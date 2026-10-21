mod hw;

use std::convert::Infallible;
use std::io;
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use axum::extract::{Path, Request, State};
use axum::http::{HeaderMap, Method, StatusCode, header};
use axum::middleware::{self, Next};
use axum::response::sse::{Event, Sse};
use axum::response::{Html, IntoResponse, Response};
use axum::routing::{get, put};
use axum::{Json, Router};
use serde::Deserialize;
use tokio::sync::{Notify, watch};
use tokio_stream::wrappers::WatchStream;
use tokio_stream::{Stream, StreamExt};

const DEFAULT_LISTEN: &str = "127.0.0.1:8930";
const INTERVAL: Duration = Duration::from_secs(1);

struct App {
    snapshot: watch::Sender<String>,
    refresh: Notify,
    hosts: Vec<String>,
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let listen: SocketAddr = match std::env::args()
        .nth(1)
        .as_deref()
        .unwrap_or(DEFAULT_LISTEN)
        .parse()
    {
        Ok(a) => a,
        Err(e) => fail(format!("地址无效：{e}")),
    };
    if !listen.ip().is_loopback() {
        fail("只能监听本机地址".into());
    }

    let port = listen.port();
    let app = Arc::new(App {
        snapshot: watch::channel(String::new()).0,
        refresh: Notify::new(),
        hosts: vec![
            listen.to_string(),
            format!("localhost:{port}"),
            format!("127.0.0.1:{port}"),
            format!("[::1]:{port}"),
        ],
    });
    tokio::spawn(sample(app.clone()));

    let router = Router::new()
        .route(
            "/",
            get(|| async { Html(include_str!("../web/index.html")) }),
        )
        .route(
            "/app.js",
            get(|| asset("text/javascript", include_str!("../web/app.js"))),
        )
        .route(
            "/style.css",
            get(|| asset("text/css", include_str!("../web/style.css"))),
        )
        .route("/api/events", get(events))
        .route("/api/profile", put(set_profile))
        .route("/api/locks/{name}", put(set_lock))
        .layer(middleware::from_fn_with_state(app.clone(), check_origin))
        .with_state(app);

    let listener = match tokio::net::TcpListener::bind(listen).await {
        Ok(l) => l,
        Err(e) => fail(format!("无法监听 {listen}：{e}")),
    };
    eprintln!("http://{listen}");
    // 没有要保存的状态，直接靠信号退出；优雅退出会一直等 SSE 连接
    axum::serve(listener, router)
        .await
        .unwrap_or_else(|e| fail(e.to_string()));
}

fn fail(msg: String) -> ! {
    eprintln!("{msg}");
    std::process::exit(1);
}

async fn asset(mime: &'static str, body: &'static str) -> impl IntoResponse {
    ([(header::CONTENT_TYPE, mime)], body)
}

/// 有页面打开时每秒采样一次，写操作后立即补一次
async fn sample(app: Arc<App>) {
    loop {
        if app.snapshot.receiver_count() > 0 {
            let json = tokio::task::spawn_blocking(|| serde_json::to_string(&hw::snapshot()))
                .await
                .expect("采样线程崩溃")
                .expect("快照无法序列化");
            app.snapshot.send_replace(json);
        }
        tokio::select! {
            _ = tokio::time::sleep(INTERVAL) => {}
            _ = app.refresh.notified() => {}
        }
    }
}

/// 只认本机的 Host，防 DNS rebinding；写请求还要求同源
async fn check_origin(
    State(app): State<Arc<App>>,
    headers: HeaderMap,
    req: Request,
    next: Next,
) -> Response {
    let host = headers
        .get(header::HOST)
        .and_then(|v| v.to_str().ok())
        .unwrap_or_default();
    if !app.hosts.iter().any(|h| h == host) {
        return StatusCode::MISDIRECTED_REQUEST.into_response();
    }
    if req.method() != Method::GET {
        let origin = headers.get(header::ORIGIN).and_then(|v| v.to_str().ok());
        if origin != Some(&format!("http://{host}")) {
            return StatusCode::FORBIDDEN.into_response();
        }
    }
    next.run(req).await
}

async fn events(State(app): State<Arc<App>>) -> Sse<impl Stream<Item = Result<Event, Infallible>>> {
    let rx = app.snapshot.subscribe();
    app.refresh.notify_one();
    let stream = WatchStream::from_changes(rx).map(|json| Ok(Event::default().data(json)));
    Sse::new(stream)
}

#[derive(Deserialize)]
struct ProfileReq {
    profile: String,
}

async fn set_profile(State(app): State<Arc<App>>, Json(req): Json<ProfileReq>) -> Response {
    write(app, move || hw::set_profile(&req.profile)).await
}

#[derive(Deserialize)]
struct LockReq {
    on: bool,
}

async fn set_lock(
    State(app): State<Arc<App>>,
    Path(name): Path<String>,
    Json(req): Json<LockReq>,
) -> Response {
    write(app, move || hw::set_lock(&name, req.on)).await
}

async fn write(app: Arc<App>, f: impl FnOnce() -> io::Result<()> + Send + 'static) -> Response {
    let res = tokio::task::spawn_blocking(f).await.expect("写入线程崩溃");
    app.refresh.notify_one();
    match res {
        Ok(()) => StatusCode::NO_CONTENT.into_response(),
        Err(e) => {
            let status = match e.kind() {
                io::ErrorKind::InvalidInput => StatusCode::BAD_REQUEST,
                io::ErrorKind::NotFound => StatusCode::SERVICE_UNAVAILABLE,
                io::ErrorKind::PermissionDenied => StatusCode::FORBIDDEN,
                // BIOS 拒绝（比如用电池时选狂暴）
                _ => StatusCode::CONFLICT,
            };
            (status, e.to_string()).into_response()
        }
    }
}

// MCP server transport — STDIO and TCP.
// Reads Content-Length headers + JSON body, dispatches to handler, writes response.

use std::io::{self, BufRead, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::Arc;
use std::thread;
use crate::session::SessionManager;
use super::handler;
use super::types::JsonRpcResponse;

/// Read one JSON-RPC request from `reader`, dispatch it, and write the response to `writer`.
pub fn handle_connection(reader: &mut dyn Read, writer: &mut dyn Write, sessions: &SessionManager) {
    let mut buf_reader = io::BufReader::new(reader);
    let mut buffer = String::new();

    loop {
        buffer.clear();

        // Read headers
        let mut content_length: Option<usize> = None;
        loop {
            let mut header = String::new();
            if buf_reader.read_line(&mut header).unwrap_or(0) == 0 {
                return; // EOF
            }
            let header = header.trim();
            if header.is_empty() {
                break; // end of headers
            }
            if let Some(len_str) = header.strip_prefix("Content-Length: ") {
                if let Ok(len) = len_str.parse::<usize>() {
                    content_length = Some(len);
                }
            }
        }

        let len = match content_length {
            Some(l) => l,
            None => continue,
        };

        if len == 0 {
            continue;
        }

        // Read JSON body
        let mut body = vec![0u8; len];
        if let Err(e) = buf_reader.read_exact(&mut body) {
            eprintln!("MCP read error: {}", e);
            return;
        }

        let body_str = match String::from_utf8(body) {
            Ok(s) => s,
            Err(_) => {
                let resp = JsonRpcResponse::parse_error(0, "invalid UTF-8");
                write_response(writer, &resp);
                continue;
            }
        };

        // Parse JSON-RPC request
        let request: super::types::JsonRpcRequest = match serde_json::from_str(&body_str) {
            Ok(req) => req,
            Err(e) => {
                let resp = JsonRpcResponse::parse_error(0, &format!("invalid JSON: {}", e));
                write_response(writer, &resp);
                continue;
            }
        };

        // Dispatch
        let response = handler::handle_request(sessions, &request.method, request.id, &request.params);
        write_response(writer, &response);
    }
}

fn write_response(writer: &mut dyn Write, response: &JsonRpcResponse) {
    let json = serde_json::to_string(response).unwrap_or_default();
    let len = json.len();
    let _ = write!(writer, "Content-Length: {}\r\n\r\n", len);
    let _ = writeln!(writer, "{}", json);
    let _ = writer.flush();
}

/// Handle a single TCP connection — wraps the stream in a buffered reader/writer.
fn handle_tcp_stream(stream: TcpStream, sessions: &SessionManager) {
    let reader: Box<dyn Read> = match stream.try_clone() {
        Ok(clone) => Box::new(clone),
        Err(e) => {
            eprintln!("MCP TCP clone error: {}", e);
            return;
        }
    };
    let mut writer: Box<dyn Write> = Box::new(stream);
    let mut reader = reader;
    handle_connection(&mut reader, &mut writer, sessions);
}

/// Start the MCP server on stdin/stdout.
/// Runs until stdin is closed.
pub fn run(sessions: &SessionManager) {
    let stdin = io::stdin();
    let stdout = io::stdout();
    let mut reader: Box<dyn Read> = Box::new(stdin.lock());
    let mut writer: Box<dyn Write> = Box::new(stdout.lock());
    handle_connection(&mut reader, &mut writer, sessions);
}

/// Start the MCP server on a TCP socket.
/// Runs until the process is killed. Each connection is handled in its own thread.
pub fn run_tcp(addr: &str, sessions: Arc<SessionManager>) -> Result<(), String> {
    let listener = TcpListener::bind(addr)
        .map_err(|e| format!("failed to bind TCP: {}", e))?;

    eprintln!("MCP TCP server listening on {}", addr);

    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                let sessions = Arc::clone(&sessions);
                thread::spawn(move || {
                    handle_tcp_stream(stream, &*sessions);
                });
            }
            Err(e) => {
                eprintln!("MCP TCP accept error: {}", e);
            }
        }
    }

    Ok(())
}

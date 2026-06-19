// MCP server — exposes simulation commands as MCP tools over STDIO.
// JSON-RPC 2.0 protocol with Content-Length header framing.

pub mod types;
pub mod handler;
pub mod server;

#[cfg(test)]
pub mod tests;

// JSON-RPC 2.0 types for the MCP server.

use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonRpcRequest {
    pub jsonrpc: String,
    pub id: u64,
    pub method: String,
    #[serde(default)]
    pub params: Option<Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonRpcResponse {
    pub jsonrpc: String,
    pub id: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub result: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<JsonRpcError>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonRpcError {
    pub code: i32,
    pub message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub data: Option<Value>,
}

impl JsonRpcResponse {
    pub fn success(id: u64, result: Value) -> Self {
        JsonRpcResponse {
            jsonrpc: "2.0".to_string(),
            id,
            result: Some(result),
            error: None,
        }
    }

    pub fn error(id: u64, code: i32, message: &str) -> Self {
        JsonRpcResponse {
            jsonrpc: "2.0".to_string(),
            id,
            result: None,
            error: Some(JsonRpcError {
                code,
                message: message.to_string(),
                data: None,
            }),
        }
    }

    pub fn parse_error(id: u64, message: &str) -> Self {
        Self::error(id, -32700, message)
    }

    pub fn invalid_request(id: u64, message: &str) -> Self {
        Self::error(id, -32600, message)
    }

    pub fn method_not_found(id: u64, method: &str) -> Self {
        Self::error(id, -32601, &format!("method not found: {}", method))
    }

    pub fn invalid_params(id: u64, message: &str) -> Self {
        Self::error(id, -32602, message)
    }

    pub fn internal_error(id: u64, message: &str) -> Self {
        Self::error(id, -32603, message)
    }
}

// Python subprocess management.
// Spawns Python scripts for regression automation, passing the MCP server
// address via the QIMING_MCP_ADDR environment variable.

use std::process::{Command, Stdio};
use std::path::Path;

/// Result of running a Python script.
#[derive(Debug)]
pub struct PythonResult {
    pub stdout: String,
    pub stderr: String,
    pub success: bool,
}

/// Manages Python subprocess execution.
pub struct PythonManager {
    script_dir: Option<String>,
    python_cmd: String,
}

impl PythonManager {
    pub fn new() -> Self {
        PythonManager {
            script_dir: None,
            python_cmd: "python".to_string(),
        }
    }

    /// Set the working directory for spawned scripts.
    pub fn with_script_dir(mut self, dir: &str) -> Self {
        self.script_dir = Some(dir.to_string());
        self
    }

    /// Override the Python interpreter command (e.g. "python3").
    pub fn with_python_cmd(mut self, cmd: &str) -> Self {
        self.python_cmd = cmd.to_string();
        self
    }

    /// Run a Python script, passing `mcp_addr` as the QIMING_MCP_ADDR env var.
    ///
    /// The script's stdout and stderr are captured and returned.
    pub fn run_script(&self, script: &str, mcp_addr: &str) -> Result<PythonResult, String> {
        let script_path = if Path::new(script).is_absolute() {
            script.to_string()
        } else if let Some(ref dir) = self.script_dir {
            Path::new(dir).join(script).to_string_lossy().to_string()
        } else {
            script.to_string()
        };

        let mut cmd = Command::new(&self.python_cmd);
        cmd.arg(&script_path)
            .env("QIMING_MCP_ADDR", mcp_addr)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());

        if let Some(ref dir) = self.script_dir {
            cmd.current_dir(dir);
        }

        let child = cmd.spawn()
            .map_err(|e| format!("failed to spawn '{} {}': {}", self.python_cmd, script, e))?;

        let output = child.wait_with_output()
            .map_err(|e| format!("failed to wait for python script: {}", e))?;

        Ok(PythonResult {
            stdout: String::from_utf8_lossy(&output.stdout).to_string(),
            stderr: String::from_utf8_lossy(&output.stderr).to_string(),
            success: output.status.success(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_python_manager_create() {
        let mgr = PythonManager::new();
        // Just verifies construction works
    }

    #[test]
    fn test_python_manager_with_dir() {
        let mgr = PythonManager::new().with_script_dir("/tmp");
        // Verifies builder works
    }

    #[test]
    fn test_python_manager_python_cmd() {
        let mgr = PythonManager::new().with_python_cmd("python3");
        // Verifies builder works
    }
}

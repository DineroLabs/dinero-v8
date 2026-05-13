// Dinero Desktop - Tauri Backend
// Phase A: Status panel + RPC connection

use anyhow::{anyhow, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{fs, path::PathBuf};
use tauri::State;

#[derive(Debug, Clone, Serialize, Deserialize)]
struct NodeInfo {
    version: String,
    network: String,
    datadir: PathBuf,
    rpc_url: String,
    rpc_port: u16,
    cookie_path: PathBuf,
    running: bool,
}

impl Default for NodeInfo {
    fn default() -> Self {
        Self {
            version: "unknown".into(),
            network: "regtest".into(),
            datadir: default_datadir("regtest"),
            rpc_url: "http://127.0.0.1:20998".into(),
            rpc_port: 20998,
            cookie_path: PathBuf::new(),
            running: false,
        }
    }
}

fn default_datadir(network: &str) -> PathBuf {
    let base = if cfg!(target_os = "macos") {
        dirs::home_dir()
            .unwrap_or_default()
            .join("Library/Application Support/Dinero")
    } else if cfg!(target_os = "windows") {
        dirs::data_dir().unwrap_or_default().join("Dinero")
    } else {
        dirs::home_dir().unwrap_or_default().join(".dinero")
    };
    
    match network {
        "regtest" => base.join("regtest"),
        "testnet" => base.join("testnet"), 
        _ => base, // mainnet
    }
}

fn load_nodeinfo() -> Result<NodeInfo> {
    // Try environment variable first
    if let Ok(path) = std::env::var("DINERO_NODEINFO") {
        return parse_nodeinfo(PathBuf::from(path));
    }
    
    // Try default locations
    let candidates = [
        default_datadir("mainnet").join("nodeinfo.json"),
        default_datadir("testnet").join("nodeinfo.json"), 
        default_datadir("regtest").join("nodeinfo.json"),
    ];
    
    for path in candidates {
        if path.exists() {
            if let Ok(info) = parse_nodeinfo(path) {
                return Ok(info);
            }
        }
    }
    
    Ok(NodeInfo::default())
}

fn parse_nodeinfo(path: PathBuf) -> Result<NodeInfo> {
    let content = fs::read_to_string(&path)?;
    let v: Value = serde_json::from_str(&content)?;
    
    let network = v["network"].as_str().unwrap_or("regtest").to_string();
    let datadir = PathBuf::from(v["datadir"].as_str().unwrap_or(""));
    let rpc_url = v["rpc"]["url"].as_str().unwrap_or("http://127.0.0.1:20998").to_string();
    let rpc_port = v["rpc"]["port"].as_u64().unwrap_or(20998) as u16;
    let cookie_path = PathBuf::from(v["auth"]["cookie_path"].as_str().unwrap_or(""));
    let running = v["status"]["running"].as_bool().unwrap_or(false);
    let version = v["version"].as_str().unwrap_or("unknown").to_string();
    
    Ok(NodeInfo {
        version,
        network,
        datadir,
        rpc_url,
        rpc_port,
        cookie_path,
        running,
    })
}

async fn rpc_call(node: &NodeInfo, method: &str, params: Value) -> Result<Value> {
    if !node.running {
        return Err(anyhow!("Node not running"));
    }
    
    // Read cookie for authentication
    let cookie = fs::read_to_string(&node.cookie_path)
        .map_err(|e| anyhow!("Failed to read cookie: {}", e))?;
    
    let client = reqwest::Client::new();
    let body = json!({
        "jsonrpc": "2.0",
        "id": "tauri",
        "method": method,
        "params": params
    });
    
    let response = client
        .post(&node.rpc_url)
        .basic_auth("", Some(cookie.trim()))
        .header("content-type", "application/json")
        .json(&body)
        .send()
        .await?;
        
    if !response.status().is_success() {
        return Err(anyhow!("RPC HTTP error: {}", response.status()));
    }
    
    let result: Value = response.json().await?;
    
    if let Some(error) = result.get("error") {
        return Err(anyhow!("RPC error: {}", error));
    }
    
    Ok(result["result"].clone())
}

#[tauri::command]
async fn get_node_info(state: State<'_, NodeInfo>) -> Result<NodeInfo, String> {
    Ok(state.inner().clone())
}

#[tauri::command] 
async fn refresh_node_info() -> Result<NodeInfo, String> {
    load_nodeinfo().map_err(|e| e.to_string())
}

#[tauri::command]
async fn rpc_request(
    method: String,
    params: Option<Value>,
    state: State<'_, NodeInfo>,
) -> Result<Value, String> {
    let params = params.unwrap_or(json!([]));
    rpc_call(state.inner(), &method, params)
        .await
        .map_err(|e| e.to_string())
}

#[tauri::command]
async fn get_blockchain_info(state: State<'_, NodeInfo>) -> Result<Value, String> {
    rpc_call(state.inner(), "getblockchaininfo", json!([]))
        .await
        .map_err(|e| e.to_string())
}

#[tauri::command]
async fn get_network_info(state: State<'_, NodeInfo>) -> Result<Value, String> {
    rpc_call(state.inner(), "getnetworkinfo", json!([]))
        .await
        .map_err(|e| e.to_string())
}

#[tokio::main]
async fn main() {
    let node_info = load_nodeinfo().unwrap_or_default();
    
    tauri::Builder::default()
        .manage(node_info)
        .plugin(tauri_plugin_log::Builder::default().build())
        .invoke_handler(tauri::generate_handler![
            get_node_info,
            refresh_node_info,
            rpc_request,
            get_blockchain_info,
            get_network_info
        ])
        .run(tauri::generate_context!())
        .expect("error while running Dinero Desktop");
}

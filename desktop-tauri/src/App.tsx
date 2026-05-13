import { useState, useEffect } from "react";
import { invoke } from "@tauri-apps/api/tauri";

interface NodeInfo {
  version: string;
  network: string;
  datadir: string;
  rpc_url: string;
  rpc_port: number;
  running: boolean;
}

interface BlockchainInfo {
  chain: string;
  blocks: number;
  headers: number;
  bestblockhash: string;
  difficulty: number;
  chainwork?: string;
  total_supply_din?: string;
  phase?: string;
}

function App() {
  const [nodeInfo, setNodeInfo] = useState<NodeInfo | null>(null);
  const [blockchainInfo, setBlockchainInfo] = useState<BlockchainInfo | null>(null);
  const [error, setError] = useState<string>("");
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    loadNodeInfo();
  }, []);

  const loadNodeInfo = async () => {
    try {
      setLoading(true);
      const info = await invoke<NodeInfo>("get_node_info");
      setNodeInfo(info);
      
      if (info.running) {
        await loadBlockchainInfo();
      }
    } catch (err) {
      setError(`Failed to load node info: ${err}`);
    } finally {
      setLoading(false);
    }
  };

  const loadBlockchainInfo = async () => {
    try {
      const info = await invoke<BlockchainInfo>("get_blockchain_info");
      setBlockchainInfo(info);
      setError("");
    } catch (err) {
      setError(`RPC Error: ${err}`);
    }
  };

  const refreshInfo = async () => {
    try {
      const info = await invoke<NodeInfo>("refresh_node_info");
      setNodeInfo(info);
      if (info.running) {
        await loadBlockchainInfo();
      } else {
        setBlockchainInfo(null);
      }
    } catch (err) {
      setError(`Refresh failed: ${err}`);
    }
  };

  if (loading) {
    return (
      <div className="app">
        <div className="loading">Loading Dinero Desktop...</div>
      </div>
    );
  }

  return (
    <div className="app">
      <header className="header">
        <h1>Dinero Desktop</h1>
        <div className={`status-pill ${nodeInfo?.running ? 'online' : 'offline'}`}>
          {nodeInfo?.running ? 'Online' : 'Offline'}
        </div>
      </header>

      <div className="content">
        <div className="card">
          <h2>Node Information</h2>
          {nodeInfo && (
            <div className="info-grid">
              <div className="info-item">
                <label>Version:</label>
                <span>{nodeInfo.version}</span>
              </div>
              <div className="info-item">
                <label>Network:</label>
                <span className={`network-badge ${nodeInfo.network}`}>
                  {nodeInfo.network}
                </span>
              </div>
              <div className="info-item">
                <label>Data Directory:</label>
                <span className="path">{nodeInfo.datadir}</span>
              </div>
              <div className="info-item">
                <label>RPC Endpoint:</label>
                <span>{nodeInfo.rpc_url}</span>
              </div>
            </div>
          )}
          <button onClick={refreshInfo} className="btn-primary">
            Refresh
          </button>
        </div>

        {nodeInfo?.running && (
          <div className="card">
            <h2>Blockchain Status</h2>
            {error && (
              <div className="error-message">{error}</div>
            )}
            {blockchainInfo && (
              <div className="info-grid">
                <div className="info-item">
                  <label>Chain:</label>
                  <span>{blockchainInfo.chain}</span>
                </div>
                <div className="info-item">
                  <label>Blocks:</label>
                  <span>{blockchainInfo.blocks.toLocaleString()}</span>
                </div>
                <div className="info-item">
                  <label>Headers:</label>
                  <span>{blockchainInfo.headers.toLocaleString()}</span>
                </div>
                <div className="info-item">
                  <label>Difficulty:</label>
                  <span>{blockchainInfo.difficulty.toFixed(6)}</span>
                </div>
                {blockchainInfo.total_supply_din && (
                  <div className="info-item">
                    <label>Total Supply:</label>
                    <span>{blockchainInfo.total_supply_din} DIN</span>
                  </div>
                )}
                {blockchainInfo.phase && (
                  <div className="info-item">
                    <label>Mining Phase:</label>
                    <span className="phase-badge">{blockchainInfo.phase}</span>
                  </div>
                )}
              </div>
            )}
            <button onClick={loadBlockchainInfo} className="btn-secondary">
              Refresh Blockchain
            </button>
          </div>
        )}

        {!nodeInfo?.running && (
          <div className="card warning">
            <h2>Node Offline</h2>
            <p>
              The Dinero daemon is not running. Please start <code>dinerod</code> 
              and ensure it's accessible at <code>{nodeInfo?.rpc_url}</code>.
            </p>
            <p>
              <strong>Data Directory:</strong> <code>{nodeInfo?.datadir}</code>
            </p>
          </div>
        )}
      </div>

      <footer className="footer">
        <p>Dinero Desktop v0.1.0 - Phase A (Status Panel)</p>
        <p>Schema: din.rpc.v1</p>
      </footer>
    </div>
  );
}

export default App;

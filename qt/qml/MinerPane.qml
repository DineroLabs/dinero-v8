import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Dinero 1.0

// Mining Control Panel
Item {
    id: root
    
    // Properties - these will be set from C++ context if available, otherwise use defaults
    property string rpcUrl: "http://127.0.0.1:20998/"
    property string dataDir: typeof DefaultDataDir !== 'undefined' ? DefaultDataDir : "./data"
    property string minerPath: typeof DefaultMinerPath !== 'undefined' ? DefaultMinerPath : "./dinero-miner"
    
    MinerController { 
        id: miner 
        
        onLogLine: function(line) {
            logArea.text += line + "\n"
            // Auto-scroll to bottom
            logArea.cursorPosition = logArea.text.length
        }
    }
    
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 15
        
        // Title
        Label {
            text: "⛏️ CPU Mining"
            font.pixelSize: 24
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
        }
        
        // Payout Address Row
        RowLayout {
            spacing: 10
            Layout.fillWidth: true
            
            Label {
                text: "Payout Address:"
                font.pixelSize: 14
                Layout.preferredWidth: 120
            }
            
            TextField {
                id: payoutAddr
                placeholderText: "din1q... (click New Address to generate)"
                font.pixelSize: 12
                Layout.fillWidth: true
                selectByMouse: true
            }
            
            Button {
                text: "New Address"
                enabled: false  // TODO: Implement RPC call
                onClicked: {
                    // TODO: Call RPC to get new address
                    // For now, user must manually paste address from Wallet tab
                    statusLabel.text = "Please generate address in Wallet tab and paste here"
                }
            }
        }
        
        // Miner Path Row
        RowLayout {
            spacing: 10
            Layout.fillWidth: true
            
            Label {
                text: "Miner Binary:"
                font.pixelSize: 14
                Layout.preferredWidth: 120
            }
            
            TextField {
                id: minerBinaryPath
                text: minerPath
                font.pixelSize: 12
                Layout.fillWidth: true
                selectByMouse: true
            }
            
            Button {
                text: "Browse..."
                onClicked: {
                    // TODO: File dialog for selecting miner binary
                    console.log("Browse for miner binary")
                }
            }
        }
        
        // Threads Row
        RowLayout {
            spacing: 10
            Layout.fillWidth: true
            
            Label {
                text: "CPU Threads:"
                font.pixelSize: 14
                Layout.preferredWidth: 120
            }
            
            SpinBox {
                id: threadsSpinBox
                from: 1
                to: 128
                value: 4  // Default
                editable: true
                Layout.preferredWidth: 150
            }
            
            Label {
                text: "(Recommended: " + Math.max(1, getProcessorCount() - 1) + " threads)"
                font.pixelSize: 11
                color: "#666"
            }
            
            Item { Layout.fillWidth: true }
        }
        
        // Start/Stop Button
        RowLayout {
            spacing: 15
            Layout.fillWidth: true
            
            Button {
                text: miner.running ? "⏸ Stop Mining" : "▶ Start Mining"
                font.pixelSize: 16
                font.bold: true
                Layout.preferredWidth: 200
                Layout.preferredHeight: 50
                
                enabled: payoutAddr.text.length > 0
                
                onClicked: {
                    if (miner.running) {
                        miner.stop()
                    } else {
                        if (payoutAddr.text.length === 0) {
                            statusLabel.text = "❌ Please set a payout address first!"
                            statusLabel.color = "red"
                            return
                        }
                        miner.start(
                            minerBinaryPath.text,
                            rpcUrl,
                            dataDir,
                            payoutAddr.text,
                            threadsSpinBox.value
                        )
                    }
                }
            }
            
            Label {
                id: statusLabel
                text: miner.status
                font.pixelSize: 14
                font.bold: true
                color: miner.running ? "green" : "gray"
                Layout.fillWidth: true
            }
        }
        
        // Stats Row
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 100
            color: "#f0f0f0"
            radius: 8
            border.color: "#ccc"
            border.width: 1
            
            RowLayout {
                anchors.fill: parent
                anchors.margins: 15
                spacing: 30
                
                // Hashrate
                Column {
                    spacing: 5
                    Label {
                        text: "Hashrate"
                        font.pixelSize: 12
                        color: "#666"
                    }
                    Label {
                        text: miner.hashrate.toFixed(2) + " H/s"
                        font.pixelSize: 20
                        font.bold: true
                        color: miner.running ? "#2196F3" : "#999"
                    }
                }
                
                Rectangle {
                    width: 1
                    Layout.fillHeight: true
                    color: "#ccc"
                }
                
                // Accepted
                Column {
                    spacing: 5
                    Label {
                        text: "Blocks Found"
                        font.pixelSize: 12
                        color: "#666"
                    }
                    Label {
                        text: miner.accepted.toString()
                        font.pixelSize: 20
                        font.bold: true
                        color: "#4CAF50"
                    }
                }
                
                Rectangle {
                    width: 1
                    Layout.fillHeight: true
                    color: "#ccc"
                }
                
                // Rejected
                Column {
                    spacing: 5
                    Label {
                        text: "Rejected"
                        font.pixelSize: 12
                        color: "#666"
                    }
                    Label {
                        text: miner.rejected.toString()
                        font.pixelSize: 20
                        font.bold: true
                        color: "#f44336"
                    }
                }
                
                Item { Layout.fillWidth: true }
            }
        }
        
        // Log Area
        Label {
            text: "Miner Log:"
            font.pixelSize: 14
            font.bold: true
        }
        
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            
            TextArea {
                id: logArea
                readOnly: true
                wrapMode: TextArea.NoWrap
                font.family: "Courier New"
                font.pixelSize: 11
                selectByMouse: true
                placeholderText: "Miner output will appear here..."
            }
        }
        
        // Clear Log Button
        Button {
            text: "Clear Log"
            Layout.alignment: Qt.AlignRight
            onClicked: {
                logArea.text = ""
            }
        }
    }
    
    // Helper function to get processor count
    function getProcessorCount() {
        // This is a placeholder - should be set from C++
        return 8
    }
    
    Component.onCompleted: {
        console.log("MinerPane loaded")
        console.log("Data dir:", dataDir)
        console.log("Miner path:", minerPath)
        console.log("RPC URL:", rpcUrl)
    }
}


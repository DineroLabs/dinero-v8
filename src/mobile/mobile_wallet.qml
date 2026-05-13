import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import Dinero.Mobile 1.0

ApplicationWindow {
    id: mainWindow
    width: 375
    height: 812
    visible: true
    title: "Dinero Mobile Wallet"
    
    // Mobile-specific properties
    property bool isPortrait: height > width
    property real scaleFactor: Math.min(width / 375, height / 812)
    
    // Wallet controller
    MobileWalletController {
        id: walletController
        onTransactionSent: {
            transactionStatus.text = "Transaction sent: " + txId
            transactionStatus.visible = true
        }
        onTransactionFailed: {
            transactionStatus.text = "Transaction failed: " + error
            transactionStatus.visible = true
        }
        onBalanceUpdated: {
            balanceLabel.text = balance.toFixed(2) + " DIN"
        }
    }
    
    // Security manager
    MobileSecurityManager {
        id: securityManager
    }
    
    // QR Code manager
    QRCodeManager {
        id: qrManager
    }
    
    // Main content
    StackView {
        id: stackView
        anchors.fill: parent
        initialItem: mainPage
        
        Component {
            id: mainPage
            
            Page {
                id: mainPageContent
                background: Rectangle {
                    color: "#f5f5f5"
                }
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 20
                    
                    // Header
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 80
                        color: "#2c3e50"
                        radius: 10
                        
                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 5
                            
                            Text {
                                text: "Dinero Wallet"
                                color: "white"
                                font.pixelSize: 24
                                font.bold: true
                                Layout.alignment: Qt.AlignHCenter
                            }
                            
                            Text {
                                text: walletController.networkStatus
                                color: "#bdc3c7"
                                font.pixelSize: 14
                                Layout.alignment: Qt.AlignHCenter
                            }
                        }
                    }
                    
                    // Balance card
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 120
                        color: "white"
                        radius: 15
                        border.color: "#e0e0e0"
                        border.width: 1
                        
                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 10
                            
                            Text {
                                text: "Balance"
                                color: "#7f8c8d"
                                font.pixelSize: 16
                                Layout.alignment: Qt.AlignHCenter
                            }
                            
                            Text {
                                id: balanceLabel
                                text: walletController.balance.toFixed(2) + " DIN"
                                color: "#2c3e50"
                                font.pixelSize: 32
                                font.bold: true
                                Layout.alignment: Qt.AlignHCenter
                            }
                        }
                    }
                    
                    // Current address
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 80
                        color: "white"
                        radius: 10
                        border.color: "#e0e0e0"
                        border.width: 1
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            spacing: 5
                            
                            Text {
                                text: "Current Address"
                                color: "#7f8c8d"
                                font.pixelSize: 14
                            }
                            
                            Text {
                                text: walletController.currentAddress
                                color: "#2c3e50"
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }
                        }
                    }
                    
                    // Action buttons
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        rowSpacing: 15
                        columnSpacing: 15
                        
                        Button {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 60
                            text: "Send"
                            background: Rectangle {
                                color: parent.pressed ? "#e74c3c" : "#e74c3c"
                                radius: 10
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: 18
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                stackView.push(sendPage)
                            }
                        }
                        
                        Button {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 60
                            text: "Receive"
                            background: Rectangle {
                                color: parent.pressed ? "#27ae60" : "#27ae60"
                                radius: 10
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: 18
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                stackView.push(receivePage)
                            }
                        }
                        
                        Button {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 60
                            text: "QR Code"
                            background: Rectangle {
                                color: parent.pressed ? "#3498db" : "#3498db"
                                radius: 10
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: 18
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                stackView.push(qrPage)
                            }
                        }
                        
                        Button {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 60
                            text: "Settings"
                            background: Rectangle {
                                color: parent.pressed ? "#9b59b6" : "#9b59b6"
                                radius: 10
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: 18
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                stackView.push(settingsPage)
                            }
                        }
                    }
                    
                    // Transaction status
                    Rectangle {
                        id: transactionStatus
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        color: "#2ecc71"
                        radius: 5
                        visible: false
                        
                        Text {
                            anchors.centerIn: parent
                            text: parent.visible ? "Transaction successful" : ""
                            color: "white"
                            font.pixelSize: 14
                        }
                        
                        Timer {
                            interval: 3000
                            running: transactionStatus.visible
                            onTriggered: transactionStatus.visible = false
                        }
                    }
                }
            }
        }
        
        Component {
            id: sendPage
            
            Page {
                id: sendPageContent
                background: Rectangle {
                    color: "#f5f5f5"
                }
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 20
                    
                    // Header
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 60
                        color: "#2c3e50"
                        radius: 10
                        
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            
                            Button {
                                text: "←"
                                background: Rectangle {
                                    color: parent.pressed ? "#34495e" : "transparent"
                                    radius: 5
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: "white"
                                    font.pixelSize: 20
                                }
                                onClicked: stackView.pop()
                            }
                            
                            Text {
                                text: "Send Dinero"
                                color: "white"
                                font.pixelSize: 20
                                font.bold: true
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                            }
                            
                            Item {
                                width: 40
                            }
                        }
                    }
                    
                    // Send form
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "white"
                        radius: 15
                        border.color: "#e0e0e0"
                        border.width: 1
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 20
                            spacing: 20
                            
                            Text {
                                text: "Send Transaction"
                                font.pixelSize: 24
                                font.bold: true
                                color: "#2c3e50"
                            }
                            
                            TextField {
                                id: toAddressField
                                Layout.fillWidth: true
                                placeholderText: "Enter recipient address"
                                background: Rectangle {
                                    color: "#f8f9fa"
                                    border.color: "#e0e0e0"
                                    border.width: 1
                                    radius: 5
                                }
                            }
                            
                            TextField {
                                id: amountField
                                Layout.fillWidth: true
                                placeholderText: "Enter amount (DIN)"
                                inputMethodHints: Qt.ImhFormattedNumbersOnly
                                background: Rectangle {
                                    color: "#f8f9fa"
                                    border.color: "#e0e0e0"
                                    border.width: 1
                                    radius: 5
                                }
                            }
                            
                            TextField {
                                id: memoField
                                Layout.fillWidth: true
                                placeholderText: "Memo (optional)"
                                background: Rectangle {
                                    color: "#f8f9fa"
                                    border.color: "#e0e0e0"
                                    border.width: 1
                                    radius: 5
                                }
                            }
                            
                            Button {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 50
                                text: "Send Transaction"
                                background: Rectangle {
                                    color: parent.pressed ? "#c0392b" : "#e74c3c"
                                    radius: 10
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: "white"
                                    font.pixelSize: 18
                                    font.bold: true
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                onClicked: {
                                    var amount = parseFloat(amountField.text)
                                    if (walletController.sendTransaction(toAddressField.text, amount, memoField.text)) {
                                        stackView.pop()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        
        Component {
            id: receivePage
            
            Page {
                id: receivePageContent
                background: Rectangle {
                    color: "#f5f5f5"
                }
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 20
                    
                    // Header
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 60
                        color: "#2c3e50"
                        radius: 10
                        
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            
                            Button {
                                text: "←"
                                background: Rectangle {
                                    color: parent.pressed ? "#34495e" : "transparent"
                                    radius: 5
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: "white"
                                    font.pixelSize: 20
                                }
                                onClicked: stackView.pop()
                            }
                            
                            Text {
                                text: "Receive Dinero"
                                color: "white"
                                font.pixelSize: 20
                                font.bold: true
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                            }
                            
                            Item {
                                width: 40
                            }
                        }
                    }
                    
                    // Receive content
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "white"
                        radius: 15
                        border.color: "#e0e0e0"
                        border.width: 1
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 20
                            spacing: 20
                            
                            Text {
                                text: "Your Address"
                                font.pixelSize: 24
                                font.bold: true
                                color: "#2c3e50"
                            }
                            
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 80
                                color: "#f8f9fa"
                                radius: 10
                                border.color: "#e0e0e0"
                                border.width: 1
                                
                                Text {
                                    anchors.centerIn: parent
                                    text: walletController.currentAddress
                                    color: "#2c3e50"
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                    width: parent.width - 20
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }
                            
                            Button {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 50
                                text: "Generate New Address"
                                background: Rectangle {
                                    color: parent.pressed ? "#229954" : "#27ae60"
                                    radius: 10
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: "white"
                                    font.pixelSize: 18
                                    font.bold: true
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                onClicked: {
                                    walletController.generateNewAddress()
                                }
                            }
                            
                            Button {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 50
                                text: "Copy Address"
                                background: Rectangle {
                                    color: parent.pressed ? "#2980b9" : "#3498db"
                                    radius: 10
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: "white"
                                    font.pixelSize: 18
                                    font.bold: true
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                onClicked: {
                                    // Copy to clipboard
                                }
                            }
                        }
                    }
                }
            }
        }
        
        Component {
            id: qrPage
            
            Page {
                id: qrPageContent
                background: Rectangle {
                    color: "#f5f5f5"
                }
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 20
                    
                    // Header
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 60
                        color: "#2c3e50"
                        radius: 10
                        
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            
                            Button {
                                text: "←"
                                background: Rectangle {
                                    color: parent.pressed ? "#34495e" : "transparent"
                                    radius: 5
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: "white"
                                    font.pixelSize: 20
                                }
                                onClicked: stackView.pop()
                            }
                            
                            Text {
                                text: "QR Code"
                                color: "white"
                                font.pixelSize: 20
                                font.bold: true
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                            }
                            
                            Item {
                                width: 40
                            }
                        }
                    }
                    
                    // QR Code content
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "white"
                        radius: 15
                        border.color: "#e0e0e0"
                        border.width: 1
                        
                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 20
                            
                            Text {
                                text: "Address QR Code"
                                font.pixelSize: 24
                                font.bold: true
                                color: "#2c3e50"
                                Layout.alignment: Qt.AlignHCenter
                            }
                            
                            Rectangle {
                                Layout.preferredWidth: 200
                                Layout.preferredHeight: 200
                                color: "white"
                                border.color: "#e0e0e0"
                                border.width: 2
                                radius: 10
                                
                                Text {
                                    anchors.centerIn: parent
                                    text: "QR Code\n" + qrManager.generateAddressQR(walletController.currentAddress)
                                    color: "#7f8c8d"
                                    font.pixelSize: 12
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }
                            
                            Button {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 50
                                text: "Scan QR Code"
                                background: Rectangle {
                                    color: parent.pressed ? "#2980b9" : "#3498db"
                                    radius: 10
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: "white"
                                    font.pixelSize: 18
                                    font.bold: true
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                onClicked: {
                                    var scannedData = qrManager.scanQRCode()
                                    // Process scanned data
                                }
                            }
                        }
                    }
                }
            }
        }
        
        Component {
            id: settingsPage
            
            Page {
                id: settingsPageContent
                background: Rectangle {
                    color: "#f5f5f5"
                }
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 20
                    
                    // Header
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 60
                        color: "#2c3e50"
                        radius: 10
                        
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 15
                            
                            Button {
                                text: "←"
                                background: Rectangle {
                                    color: parent.pressed ? "#34495e" : "transparent"
                                    radius: 5
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: "white"
                                    font.pixelSize: 20
                                }
                                onClicked: stackView.pop()
                            }
                            
                            Text {
                                text: "Settings"
                                color: "white"
                                font.pixelSize: 20
                                font.bold: true
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                            }
                            
                            Item {
                                width: 40
                            }
                        }
                    }
                    
                    // Settings content
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "white"
                        radius: 15
                        border.color: "#e0e0e0"
                        border.width: 1
                        
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 20
                            spacing: 20
                            
                            Text {
                                text: "Wallet Settings"
                                font.pixelSize: 24
                                font.bold: true
                                color: "#2c3e50"
                            }
                            
                            Switch {
                                text: "Biometric Authentication"
                                checked: securityManager.biometricEnabled
                                onToggled: {
                                    if (checked) {
                                        securityManager.enableBiometricAuth()
                                    } else {
                                        securityManager.disableBiometricAuth()
                                    }
                                }
                            }
                            
                            Switch {
                                text: "Screen Lock"
                                checked: securityManager.screenLockEnabled
                                onToggled: {
                                    if (checked) {
                                        securityManager.enableScreenLock()
                                    } else {
                                        securityManager.disableScreenLock()
                                    }
                                }
                            }
                            
                            Button {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 50
                                text: "Connect to Network"
                                background: Rectangle {
                                    color: parent.pressed ? "#229954" : "#27ae60"
                                    radius: 10
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: "white"
                                    font.pixelSize: 18
                                    font.bold: true
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                onClicked: {
                                    walletController.connectToNetwork()
                                }
                            }
                            
                            Button {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 50
                                text: "Disconnect from Network"
                                background: Rectangle {
                                    color: parent.pressed ? "#c0392b" : "#e74c3c"
                                    radius: 10
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: "white"
                                    font.pixelSize: 18
                                    font.bold: true
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                onClicked: {
                                    walletController.disconnectFromNetwork()
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

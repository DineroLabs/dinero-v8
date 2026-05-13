/**
 * Multi-Account GUI Integration
 * 
 * This module provides Qt GUI components for managing multiple HD wallet accounts
 */

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

ApplicationWindow {
    id: multiAccountWindow
    width: 800
    height: 600
    visible: true
    title: "Dinero Multi-Account Manager"
    
    property string currentAccountId: ""
    property var accountManager: null
    
    // Account Manager (would be connected to C++ backend)
    Component.onCompleted: {
        // Initialize account manager
        accountManager = Qt.createQmlObject('
            import QtQuick 2.15
            QtObject {
                property var accounts: []
                property string currentAccount: ""
                
                function createAccount(name, description, type, color) {
                    var accountId = "acc_" + Math.random().toString(36).substr(2, 9);
                    var account = {
                        id: accountId,
                        name: name,
                        description: description,
                        type: type,
                        color: color,
                        balance: 0.0,
                        address: "din1q" + Math.random().toString(36).substr(2, 20),
                        createdAt: new Date().toISOString()
                    };
                    accounts.push(account);
                    return accountId;
                }
                
                function switchToAccount(accountId) {
                    currentAccount = accountId;
                }
                
                function getAccount(accountId) {
                    for (var i = 0; i < accounts.length; i++) {
                        if (accounts[i].id === accountId) {
                            return accounts[i];
                        }
                    }
                    return null;
                }
                
                function generateNewAddress(accountId) {
                    var account = getAccount(accountId);
                    if (account) {
                        account.address = "din1q" + Math.random().toString(36).substr(2, 20);
                        return account.address;
                    }
                    return "";
                }
                
                function sendTransaction(accountId, toAddress, amount, memo) {
                    var account = getAccount(accountId);
                    if (account && amount <= account.balance) {
                        account.balance -= amount;
                        return true;
                    }
                    return false;
                }
            }
        ', multiAccountWindow);
        
        // Create default accounts
        accountManager.createAccount("Personal", "My personal Dinero wallet", "PERSONAL", "#3498db");
        accountManager.createAccount("Business", "Business transactions", "BUSINESS", "#e74c3c");
        accountManager.createAccount("Savings", "Long-term savings", "SAVINGS", "#27ae60");
        
        currentAccountId = accountManager.accounts[0].id;
        accountManager.switchToAccount(currentAccountId);
    }
    
    // Main content
    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10
        
        // Account List Panel
        Rectangle {
            Layout.preferredWidth: 250
            Layout.fillHeight: true
            color: "#f8f9fa"
            border.color: "#e0e0e0"
            border.width: 1
            radius: 8
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 15
                spacing: 15
                
                // Header
                Text {
                    text: "Accounts"
                    font.pixelSize: 18
                    font.bold: true
                    color: "#2c3e50"
                }
                
                // Add Account Button
                Button {
                    Layout.fillWidth: true
                    text: "+ Add Account"
                    background: Rectangle {
                        color: parent.pressed ? "#2980b9" : "#3498db"
                        radius: 5
                    }
                    contentItem: Text {
                        text: parent.text
                        color: "white"
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        addAccountDialog.open();
                    }
                }
                
                // Account List
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    
                    ListView {
                        id: accountListView
                        model: accountManager ? accountManager.accounts : []
                        spacing: 8
                        
                        delegate: Rectangle {
                            width: accountListView.width
                            height: 80
                            color: modelData.id === currentAccountId ? "#e3f2fd" : "white"
                            border.color: modelData.id === currentAccountId ? "#2196f3" : "#e0e0e0"
                            border.width: modelData.id === currentAccountId ? 2 : 1
                            radius: 8
                            
                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 10
                                
                                // Account Icon
                                Rectangle {
                                    Layout.preferredWidth: 40
                                    Layout.preferredHeight: 40
                                    color: modelData.color
                                    radius: 20
                                    
                                    Text {
                                        anchors.centerIn: parent
                                        text: getAccountIcon(modelData.type)
                                        color: "white"
                                        font.pixelSize: 16
                                    }
                                }
                                
                                // Account Info
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    
                                    Text {
                                        text: modelData.name
                                        font.pixelSize: 14
                                        font.bold: true
                                        color: "#2c3e50"
                                    }
                                    
                                    Text {
                                        text: modelData.description
                                        font.pixelSize: 12
                                        color: "#7f8c8d"
                                        elide: Text.ElideRight
                                    }
                                    
                                    Text {
                                        text: modelData.balance.toFixed(2) + " DIN"
                                        font.pixelSize: 12
                                        font.bold: true
                                        color: "#27ae60"
                                    }
                                }
                                
                                // Actions
                                ColumnLayout {
                                    spacing: 5
                                    
                                    Button {
                                        text: "Switch"
                                        background: Rectangle {
                                            color: parent.pressed ? "#2980b9" : "#3498db"
                                            radius: 3
                                        }
                                        contentItem: Text {
                                            text: parent.text
                                            color: "white"
                                            font.pixelSize: 10
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                        onClicked: {
                                            currentAccountId = modelData.id;
                                            accountManager.switchToAccount(modelData.id);
                                        }
                                    }
                                    
                                    Button {
                                        text: "Settings"
                                        background: Rectangle {
                                            color: parent.pressed ? "#95a5a6" : "#bdc3c7"
                                            radius: 3
                                        }
                                        contentItem: Text {
                                            text: parent.text
                                            color: "white"
                                            font.pixelSize: 10
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                        onClicked: {
                                            accountSettingsDialog.accountId = modelData.id;
                                            accountSettingsDialog.open();
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        
        // Main Account Panel
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "white"
            border.color: "#e0e0e0"
            border.width: 1
            radius: 8
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 20
                
                // Account Header
                RowLayout {
                    Layout.fillWidth: true
                    
                    Rectangle {
                        Layout.preferredWidth: 60
                        Layout.preferredHeight: 60
                        color: getCurrentAccountColor()
                        radius: 30
                        
                        Text {
                            anchors.centerIn: parent
                            text: getCurrentAccountIcon()
                            color: "white"
                            font.pixelSize: 24
                        }
                    }
                    
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        
                        Text {
                            text: getCurrentAccountName()
                            font.pixelSize: 24
                            font.bold: true
                            color: "#2c3e50"
                        }
                        
                        Text {
                            text: getCurrentAccountDescription()
                            font.pixelSize: 14
                            color: "#7f8c8d"
                        }
                        
                        Text {
                            text: "Balance: " + getCurrentAccountBalance().toFixed(2) + " DIN"
                            font.pixelSize: 16
                            font.bold: true
                            color: "#27ae60"
                        }
                    }
                    
                    Button {
                        text: "Generate Address"
                        background: Rectangle {
                            color: parent.pressed ? "#27ae60" : "#2ecc71"
                            radius: 5
                        }
                        contentItem: Text {
                            text: parent.text
                            color: "white"
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: {
                            var newAddress = accountManager.generateNewAddress(currentAccountId);
                            currentAddressText.text = newAddress;
                        }
                    }
                }
                
                // Current Address
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 60
                    color: "#f8f9fa"
                    border.color: "#e0e0e0"
                    border.width: 1
                    radius: 5
                    
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 10
                        
                        Text {
                            text: "Current Address:"
                            font.pixelSize: 14
                            color: "#2c3e50"
                        }
                        
                        Text {
                            id: currentAddressText
                            Layout.fillWidth: true
                            text: getCurrentAccountAddress()
                            font.pixelSize: 12
                            font.family: "Courier New"
                            color: "#2c3e50"
                            elide: Text.ElideMiddle
                        }
                        
                        Button {
                            text: "Copy"
                            background: Rectangle {
                                color: parent.pressed ? "#2980b9" : "#3498db"
                                radius: 3
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: 12
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                // Copy address to clipboard
                                console.log("Copying address:", currentAddressText.text);
                            }
                        }
                    }
                }
                
                // Send Transaction
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 200
                    color: "#f8f9fa"
                    border.color: "#e0e0e0"
                    border.width: 1
                    radius: 5
                    
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 15
                        spacing: 10
                        
                        Text {
                            text: "Send Transaction"
                            font.pixelSize: 16
                            font.bold: true
                            color: "#2c3e50"
                        }
                        
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            
                            Text {
                                text: "To:"
                                font.pixelSize: 14
                                color: "#2c3e50"
                            }
                            
                            TextField {
                                id: toAddressField
                                Layout.fillWidth: true
                                placeholderText: "Enter recipient address"
                                background: Rectangle {
                                    color: "white"
                                    border.color: "#e0e0e0"
                                    border.width: 1
                                    radius: 3
                                }
                            }
                        }
                        
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            
                            Text {
                                text: "Amount:"
                                font.pixelSize: 14
                                color: "#2c3e50"
                            }
                            
                            TextField {
                                id: amountField
                                Layout.fillWidth: true
                                placeholderText: "0.00"
                                inputMethodHints: Qt.ImhFormattedNumbersOnly
                                background: Rectangle {
                                    color: "white"
                                    border.color: "#e0e0e0"
                                    border.width: 1
                                    radius: 3
                                }
                            }
                            
                            Text {
                                text: "DIN"
                                font.pixelSize: 14
                                color: "#2c3e50"
                            }
                        }
                        
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            
                            Text {
                                text: "Memo:"
                                font.pixelSize: 14
                                color: "#2c3e50"
                            }
                            
                            TextField {
                                id: memoField
                                Layout.fillWidth: true
                                placeholderText: "Optional memo"
                                background: Rectangle {
                                    color: "white"
                                    border.color: "#e0e0e0"
                                    border.width: 1
                                    radius: 3
                                }
                            }
                        }
                        
                        Button {
                            Layout.fillWidth: true
                            text: "Send Transaction"
                            background: Rectangle {
                                color: parent.pressed ? "#c0392b" : "#e74c3c"
                                radius: 5
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: 14
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                var amount = parseFloat(amountField.text);
                                var success = accountManager.sendTransaction(
                                    currentAccountId, 
                                    toAddressField.text, 
                                    amount, 
                                    memoField.text
                                );
                                
                                if (success) {
                                    console.log("Transaction sent successfully");
                                    toAddressField.text = "";
                                    amountField.text = "";
                                    memoField.text = "";
                                } else {
                                    console.log("Transaction failed");
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Add Account Dialog
    Dialog {
        id: addAccountDialog
        title: "Add New Account"
        width: 400
        height: 300
        
        ColumnLayout {
            anchors.fill: parent
            spacing: 15
            
            TextField {
                id: accountNameField
                Layout.fillWidth: true
                placeholderText: "Account Name"
                background: Rectangle {
                    color: "white"
                    border.color: "#e0e0e0"
                    border.width: 1
                    radius: 3
                }
            }
            
            TextField {
                id: accountDescriptionField
                Layout.fillWidth: true
                placeholderText: "Account Description"
                background: Rectangle {
                    color: "white"
                    border.color: "#e0e0e0"
                    border.width: 1
                    radius: 3
                }
            }
            
            ComboBox {
                id: accountTypeCombo
                Layout.fillWidth: true
                model: ["PERSONAL", "BUSINESS", "SAVINGS", "INVESTMENT", "FAMILY", "CHARITY", "CUSTOM"]
                background: Rectangle {
                    color: "white"
                    border.color: "#e0e0e0"
                    border.width: 1
                    radius: 3
                }
            }
            
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                
                Text {
                    text: "Color:"
                    font.pixelSize: 14
                    color: "#2c3e50"
                }
                
                Rectangle {
                    Layout.preferredWidth: 30
                    Layout.preferredHeight: 30
                    color: accountColorField.text
                    border.color: "#e0e0e0"
                    border.width: 1
                    radius: 15
                }
                
                TextField {
                    id: accountColorField
                    Layout.fillWidth: true
                    text: "#3498db"
                    placeholderText: "#3498db"
                    background: Rectangle {
                        color: "white"
                        border.color: "#e0e0e0"
                        border.width: 1
                        radius: 3
                    }
                }
            }
            
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                
                Button {
                    Layout.fillWidth: true
                    text: "Cancel"
                    background: Rectangle {
                        color: parent.pressed ? "#95a5a6" : "#bdc3c7"
                        radius: 5
                    }
                    contentItem: Text {
                        text: parent.text
                        color: "white"
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        addAccountDialog.close();
                    }
                }
                
                Button {
                    Layout.fillWidth: true
                    text: "Create Account"
                    background: Rectangle {
                        color: parent.pressed ? "#2980b9" : "#3498db"
                        radius: 5
                    }
                    contentItem: Text {
                        text: parent.text
                        color: "white"
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        var accountId = accountManager.createAccount(
                            accountNameField.text,
                            accountDescriptionField.text,
                            accountTypeCombo.currentText,
                            accountColorField.text
                        );
                        
                        if (accountId) {
                            currentAccountId = accountId;
                            accountManager.switchToAccount(accountId);
                            addAccountDialog.close();
                            
                            // Clear fields
                            accountNameField.text = "";
                            accountDescriptionField.text = "";
                            accountColorField.text = "#3498db";
                        }
                    }
                }
            }
        }
    }
    
    // Account Settings Dialog
    Dialog {
        id: accountSettingsDialog
        title: "Account Settings"
        width: 400
        height: 300
        
        property string accountId: ""
        
        ColumnLayout {
            anchors.fill: parent
            spacing: 15
            
            Text {
                text: "Account ID: " + accountSettingsDialog.accountId
                font.pixelSize: 12
                color: "#7f8c8d"
            }
            
            TextField {
                id: settingsNameField
                Layout.fillWidth: true
                placeholderText: "Account Name"
                background: Rectangle {
                    color: "white"
                    border.color: "#e0e0e0"
                    border.width: 1
                    radius: 3
                }
            }
            
            TextField {
                id: settingsDescriptionField
                Layout.fillWidth: true
                placeholderText: "Account Description"
                background: Rectangle {
                    color: "white"
                    border.color: "#e0e0e0"
                    border.width: 1
                    radius: 3
                }
            }
            
            Button {
                Layout.fillWidth: true
                text: "Export Account"
                background: Rectangle {
                    color: parent.pressed ? "#27ae60" : "#2ecc71"
                    radius: 5
                }
                contentItem: Text {
                    text: parent.text
                    color: "white"
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    console.log("Exporting account:", accountSettingsDialog.accountId);
                }
            }
            
            Button {
                Layout.fillWidth: true
                text: "Delete Account"
                background: Rectangle {
                    color: parent.pressed ? "#c0392b" : "#e74c3c"
                    radius: 5
                }
                contentItem: Text {
                    text: parent.text
                    color: "white"
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    console.log("Deleting account:", accountSettingsDialog.accountId);
                    accountSettingsDialog.close();
                }
            }
        }
    }
    
    // Helper Functions
    function getCurrentAccount() {
        if (accountManager) {
            return accountManager.getAccount(currentAccountId);
        }
        return null;
    }
    
    function getCurrentAccountName() {
        var account = getCurrentAccount();
        return account ? account.name : "No Account";
    }
    
    function getCurrentAccountDescription() {
        var account = getCurrentAccount();
        return account ? account.description : "";
    }
    
    function getCurrentAccountBalance() {
        var account = getCurrentAccount();
        return account ? account.balance : 0.0;
    }
    
    function getCurrentAccountAddress() {
        var account = getCurrentAccount();
        return account ? account.address : "din1...";
    }
    
    function getCurrentAccountColor() {
        var account = getCurrentAccount();
        return account ? account.color : "#3498db";
    }
    
    function getCurrentAccountIcon() {
        var account = getCurrentAccount();
        return getAccountIcon(account ? account.type : "PERSONAL");
    }
    
    function getAccountIcon(type) {
        switch (type) {
            case "PERSONAL": return "👤";
            case "BUSINESS": return "🏢";
            case "SAVINGS": return "💰";
            case "INVESTMENT": return "📈";
            case "FAMILY": return "👨‍👩‍👧‍👦";
            case "CHARITY": return "❤️";
            case "CUSTOM": return "⚙️";
            default: return "👤";
        }
    }
}

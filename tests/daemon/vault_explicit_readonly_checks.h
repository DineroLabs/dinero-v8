#pragma once
// Benign policy-only callback: no wallet state or vault attachment is invoked.
TEST(RpcRequestFraming, VaultCreationRequiresAdminAccess) {
    for(bool readonly:{true,false}) {
        const auto port=Port();HttpRpcServer server("127.0.0.1",port);
        auto auth=std::make_shared<RpcAuth>("/unused-vault-creation-policy-fixture");
        auth->set_static_credentials("test","secret");server.set_auth(auth);
        server.set_readonly_mode(readonly);std::atomic<unsigned> calls{0};
        server.register_method("vault.create",[&](const Json::Value&){++calls;return Json::Value("policy-only");});
        server.start();
        {Socket client;Connect(client,port);SendFrame(client.fd,Body("policy-only","vault.create"),"Authorization: Basic dGVzdDpzZWNyZXQ=\r\n");
         const auto response=Response(Read(client.fd),200);
         if(readonly) {
             EXPECT_TRUE(response["error"].isObject());
             EXPECT_NE(response["error"]["message"].asString().find("admin-only"),std::string::npos);
         } else EXPECT_EQ(response["result"].asString(),"policy-only");}
        server.stop();EXPECT_EQ(calls.load(),readonly?0u:1u);
    }
}

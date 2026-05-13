#include "privacy/payjoin_receiver.h"
#include "daemon/execution_context.h"
#include <json/json.h>

namespace din::http {

static std::string get_form_field(const std::string& body, const std::string& key) {
  // ultra-simple form parser: key=value&key2=...
  auto pos = body.find(key + "=");
  if (pos == std::string::npos) return {};
  pos += key.size() + 1;
  auto end = body.find('&', pos);
  auto val = body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  // naive URL decode of '+' and %3D only (PSBT base64 often has '=')
  std::string out; out.reserve(val.size());
  for (size_t i=0;i<val.size();++i) {
    if (val[i]=='+') out.push_back(' ');
    else if (val[i]=='%' && i+2<val.size()) {
      auto hexv=[&](char c)->int{ if('0'<=c&&c<='9') return c-'0'; if('a'<=c&&c<='f') return c-'a'+10; if('A'<=c&&c<='F') return c-'A'+10; return -1; };
      int h1=hexv(val[i+1]), h2=hexv(val[i+2]); if(h1>=0 && h2>=0){ out.push_back(char((h1<<4)|h2)); i+=2; } else out.push_back(val[i]);
    } else out.push_back(val[i]);
  }
  return out;
}

// Register this with your Beast router for POST /payjoin
std::string handle_payjoin_post(ExecutionContext& ctx,
                                din::PayjoinReceiver& receiver,
                                const std::string& content_type,
                                const std::string& body) {
  try {
    std::string psbt_b64;
    int64_t minfeerate = 0;
    bool disable_out_sub = true;

    if (content_type.find("application/json") != std::string::npos) {
      Json::CharReaderBuilder b; Json::Value j; std::string errs;
      auto* r = b.newCharReader();
      if (!r->parse(body.data(), body.data()+body.size(), &j, &errs)) {
        throw std::runtime_error("bad json");
      }
      delete r;
      psbt_b64 = j.get("psbt","").asString();
      minfeerate = j.get("minfeerate", 0).asInt64();
      disable_out_sub = j.get("disableoutputsubstitution", true).asBool();
    } else {
      // default to form
      psbt_b64 = get_form_field(body, "psbt");
      auto mf = get_form_field(body, "minfeerate"); if (!mf.empty()) minfeerate = std::stoll(mf);
      auto dos = get_form_field(body, "disableoutputsubstitution");
      if (!dos.empty()) disable_out_sub = (dos=="1" || dos=="true" || dos=="on");
    }

    if (psbt_b64.empty()) throw std::runtime_error("missing psbt");

    std::string out_b64 = receiver.handle(psbt_b64, minfeerate, disable_out_sub);

    // text/plain is common for BIP78 responses
    return out_b64;
  } catch (const std::exception& e) {
    // You can map specific errors to HTTP 4xx/5xx in your server layer; here we bubble a message.
    throw;
  }
}

} // namespace din::http

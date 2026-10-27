#include "storage/validated_tip_transition.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace dinero;
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int main() try {
    char temp[]="/tmp/dinero-validated-transition-XXXXXX";
    Check(mkdtemp(temp),"mkdtemp");
    const auto token=ChainWriteToken::CreateForTesting();
    uint256 parent,child,other;
    parent.data[0]=4;child.data[0]=5;other.data[0]=9;
    {
        ChainDB db;Check(db.init(temp)==Status::Ok,"open");
        // An absent marker must never be synthesized from a canonical tip.
        rocksdb::WriteBatch missing;
        Check(storage::StageMatchingValidatedTip(db,token,parent,4,child,5,missing)==Status::Ok,"stage absent");
        Check(db.writeBatch(token,std::move(missing),true)==Status::Ok,"commit absent");
        Check(db.getValidatedTip().status()==Status::NotFound,"manufactured validation history");
        Check(db.setValidatedTip(token,child,5)==Status::Ok,"seed child marker");
        rocksdb::WriteBatch rollback;
        Check(storage::StageMatchingValidatedTip(db,token,child,5,parent,4,rollback)==Status::Ok,"stage rollback");
        Check(db.getValidatedTip()->hash==child,"rollback visible before canonical commit");
        Check(db.setTip(token,parent,4,arith_uint256(4),&rollback)==Status::Ok,"stage canonical rollback");
        Check(db.writeBatch(token,std::move(rollback),true)==Status::Ok,"commit rollback");
        Check(db.getValidatedTip()->hash==parent && db.getValidatedTip()->height==4,"marker not rolled back");
        Check(db.getTip()->hash==db.getValidatedTip()->hash,"split rollback markers");
        rocksdb::WriteBatch replay;
        Check(storage::StageMatchingValidatedTip(db,token,parent,4,child,5,replay)==Status::Ok,"stage replay");
        Check(db.getValidatedTip()->hash==parent,"replay visible before canonical commit");
        Check(db.setTip(token,child,5,arith_uint256(5),&replay)==Status::Ok,"stage canonical replay");
        Check(db.writeBatch(token,std::move(replay),true)==Status::Ok,"commit replay");
        Check(db.getValidatedTip()->hash==child && db.getValidatedTip()->height==5,"marker not advanced");
    }
    {
        ChainDB db;Check(db.init(temp)==Status::Ok,"reopen");
        Check(db.getTip()->hash==child && db.getValidatedTip()->hash==child,"restart lost atomic marker pair");
        for (const auto& marker : {std::pair{parent,0},std::pair{other,5},std::pair{child,4}}) {
            Check(db.setValidatedTip(token,marker.first,marker.second)==Status::Ok,"seed mismatched marker");
            rocksdb::WriteBatch batch;
            Check(storage::StageMatchingValidatedTip(db,token,child,5,parent,4,batch)==Status::Ok,"stage mismatch");
            Check(db.writeBatch(token,std::move(batch),true)==Status::Ok,"commit mismatch");
            const auto got=db.getValidatedTip();
            Check(got.ok() && got->hash==marker.first && got->height==marker.second,"guessed validation progress");
        }
    }
    std::filesystem::remove_all(temp);
    std::cout<<"PASS atomic validation-tip rollback/replay, restart, absent and mismatched markers\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}

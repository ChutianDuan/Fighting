# include <lab/net/NetStub.h>
# include <lab/util/log.h>

std::optional<InputCmd> NetStub::Recv(){
    return std::nullopt;
}

void NetStub::Send(const InputCmd &cmd){
    (void)cmd;
    LOGE("Send tick=%u buttons=%u moveX=%d", cmd.tick, cmd.buttons, int(cmd.moveX));
}
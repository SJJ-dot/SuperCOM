#pragma once
// 单文件自更新：新版本 exe 以 `--self-update <旧进程PID>` 启动，
// 等旧进程退出后把自身复制覆盖原 exe，再拉起新 exe。失败会回滚并写日志。

namespace selfupdate {

// 若命令行带 --self-update，执行更新并返回 true（调用方应立即退出，不再创建界面）。
// 返回 false 表示普通启动。
bool runIfRequested(int argc, char** argv);

// 普通启动时调用：若上次更新成功（存在成功标记），删掉更新器 exe、日志等残留文件。
void cleanupAfterUpdate();

}   // namespace selfupdate

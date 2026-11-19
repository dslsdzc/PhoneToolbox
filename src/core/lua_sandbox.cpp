#include "lua_sandbox.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace {

LuaSandbox *sandboxOf(lua_State *L)
{
    return *static_cast<LuaSandbox **>(lua_getextraspace(L));
}

void instructionHook(lua_State *L, lua_Debug *)
{
    LuaSandbox *sandbox = sandboxOf(L);
    if (sandbox && sandbox->timedOut())
        luaL_error(L, "脚本执行超时（超过 %d ms）", LuaSandbox::kTimeoutMs);
}

int luaUiSetHtml(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    size_t length = 0;
    const char *html = luaL_checklstring(L, 1, &length);
    if (length > size_t(LuaSandbox::kMaxHtmlBytes))
        return luaL_error(L, "HTML 超过 %d 字节上限", LuaSandbox::kMaxHtmlBytes);
    sandbox->setHtml(QString::fromUtf8(html, int(length)));
    return 0;
}

int luaUiAction(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const char *id = luaL_checkstring(L, 1);
    const char *label = luaL_optstring(L, 2, id);
    sandbox->addAction(QString::fromUtf8(id), QString::fromUtf8(label));
    return 0;
}

int luaUiInput(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const char *id = luaL_checkstring(L, 1);
    const char *label = luaL_optstring(L, 2, id);
    const char *defaultValue = luaL_optstring(L, 3, "");
    sandbox->addInput({QString::fromUtf8(id), QString::fromUtf8(label),
                       QString::fromUtf8(defaultValue), false});
    return 0;
}

int luaUiCheckbox(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const char *id = luaL_checkstring(L, 1);
    const char *label = luaL_optstring(L, 2, id);
    const bool defaultValue = lua_toboolean(L, 3) != 0;
    sandbox->addInput({QString::fromUtf8(id), QString::fromUtf8(label),
                       defaultValue ? QStringLiteral("1") : QStringLiteral("0"), true});
    return 0;
}

int luaUiStatus(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const char *id = luaL_checkstring(L, 1);
    const char *text = luaL_optstring(L, 2, "");
    sandbox->setStatus(QString::fromUtf8(id), QString::fromUtf8(text));
    return 0;
}

int luaUiValue(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const char *id = luaL_checkstring(L, 1);
    lua_pushstring(L, sandbox->value(QString::fromUtf8(id)).toUtf8().constData());
    return 1;
}

int luaUiSetEnabled(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const char *id = luaL_checkstring(L, 1);
    const bool enabled = lua_toboolean(L, 2) != 0;
    sandbox->setActionEnabled(QString::fromUtf8(id), enabled);
    return 0;
}

int luaBromScan(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const LuaSandbox::BromApi &api = sandbox->bromApi();
    if (!api.scan) {
        lua_pushstring(L, "BROM 设备枚举未接入");
        return 1;
    }
    lua_pushstring(L, api.scan().toUtf8().constData());
    return 1;
}

int luaBromChipStatus(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const LuaSandbox::BromApi &api = sandbox->bromApi();
    const char *hwCode = luaL_checkstring(L, 1);
    const char *var1 = luaL_optstring(L, 2, "");
    if (!api.chipStatus) {
        lua_pushstring(L, "芯片表查询未接入");
        return 1;
    }
    lua_pushstring(L, api.chipStatus(QString::fromUtf8(hwCode), QString::fromUtf8(var1))
                          .toUtf8().constData());
    return 1;
}

int luaBromGate(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const LuaSandbox::BromApi &api = sandbox->bromApi();
    const char *hwCode = luaL_checkstring(L, 1);
    const char *var1 = luaL_optstring(L, 2, "");
    const bool payloadReady = lua_toboolean(L, 3) != 0;

    LuaSandbox::BromGate gate;
    if (api.gate)
        gate = api.gate(QString::fromUtf8(hwCode), QString::fromUtf8(var1), payloadReady);
    else
        gate.reason = QStringLiteral("执行闸门未接入");

    lua_newtable(L);
    lua_pushboolean(L, gate.canRun);
    lua_setfield(L, -2, "can_run");
    lua_pushstring(L, gate.chipSummary.toUtf8().constData());
    lua_setfield(L, -2, "chip_summary");
    lua_pushstring(L, gate.reason.toUtf8().constData());
    lua_setfield(L, -2, "message");
    return 1;
}

int luaBromResolve(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const LuaSandbox::BromApi &api = sandbox->bromApi();
    const char *hwCode = luaL_checkstring(L, 1);
    const char *payloadPath = luaL_optstring(L, 2, "");
    const bool allowNetwork = lua_toboolean(L, 3) != 0;

    QString message;
    bool ok = false;
    if (api.resolve)
        ok = api.resolve(QString::fromUtf8(hwCode), QString::fromUtf8(payloadPath),
                         allowNetwork, &message);
    else
        message = QStringLiteral("载荷解析未接入");

    lua_pushboolean(L, ok);
    lua_pushstring(L, message.toUtf8().constData());
    return 2;
}

int luaBromRun(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const LuaSandbox::BromApi &api = sandbox->bromApi();
    const char *hwCode = luaL_checkstring(L, 1);
    const char *var1 = luaL_optstring(L, 2, "");
    const bool allowPatched = lua_toboolean(L, 3) != 0;

    QString message;
    bool ok = false;
    if (api.run)
        ok = api.run(QString::fromUtf8(hwCode), QString::fromUtf8(var1), allowPatched,
                     &message);
    else
        message = QStringLiteral("BROM 执行未接入");

    lua_pushboolean(L, ok);
    lua_pushstring(L, message.toUtf8().constData());
    return 2;
}

int luaLog(lua_State *L)
{
    LuaSandbox *sandbox = sandboxOf(L);
    const char *text = luaL_checkstring(L, 1);
    sandbox->log(QString::fromUtf8(text).left(4096));
    return 0;
}

} // namespace

LuaSandbox::LuaSandbox()
{
    m_state = luaL_newstate();
    if (!m_state)
        return;

    *static_cast<LuaSandbox **>(lua_getextraspace(m_state)) = this;
    registerApi();
}

LuaSandbox::~LuaSandbox()
{
    if (m_state)
        lua_close(m_state);
}

void LuaSandbox::registerApi()
{
    // 白名单库。**不要**换成 luaL_openlibs：那会把 io/os/package/debug 一起打开。
    luaL_requiref(m_state, "_G", luaopen_base, 1);
    luaL_requiref(m_state, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(m_state, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(m_state, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(m_state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(m_state, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_settop(m_state, 0);

    // base 库自带文件加载入口：不关掉等于把宿主机文件系统暴露给漏洞库脚本。
    for (const char *name : {"dofile", "loadfile", "load"}) {
        lua_pushnil(m_state);
        lua_setglobal(m_state, name);
    }
    // print 写到 stdout 没有意义；统一走 log（输出面板）。
    lua_pushnil(m_state);
    lua_setglobal(m_state, "print");

    lua_newtable(m_state);
    lua_pushcfunction(m_state, luaUiSetHtml);
    lua_setfield(m_state, -2, "set_html");
    lua_pushcfunction(m_state, luaUiAction);
    lua_setfield(m_state, -2, "action");
    lua_pushcfunction(m_state, luaUiInput);
    lua_setfield(m_state, -2, "input");
    lua_pushcfunction(m_state, luaUiCheckbox);
    lua_setfield(m_state, -2, "checkbox");
    lua_pushcfunction(m_state, luaUiStatus);
    lua_setfield(m_state, -2, "status");
    lua_pushcfunction(m_state, luaUiStatus);
    lua_setfield(m_state, -2, "set_status");
    lua_pushcfunction(m_state, luaUiValue);
    lua_setfield(m_state, -2, "value");
    lua_pushcfunction(m_state, luaUiSetEnabled);
    lua_setfield(m_state, -2, "set_enabled");
    lua_setglobal(m_state, "ui");

    // BROM 能力：界面/流程写在 Lua，宿主只提供底层操作。
    lua_newtable(m_state);
    lua_pushcfunction(m_state, luaBromScan);
    lua_setfield(m_state, -2, "scan");
    lua_pushcfunction(m_state, luaBromChipStatus);
    lua_setfield(m_state, -2, "chip_status");
    lua_pushcfunction(m_state, luaBromGate);
    lua_setfield(m_state, -2, "gate");
    lua_pushcfunction(m_state, luaBromResolve);
    lua_setfield(m_state, -2, "resolve");
    lua_pushcfunction(m_state, luaBromRun);
    lua_setfield(m_state, -2, "run");
    lua_setglobal(m_state, "brom");

    lua_pushcfunction(m_state, luaLog);
    lua_setglobal(m_state, "log");
}

void LuaSandbox::pushDeviceTable()
{
    lua_newtable(m_state);
    const auto field = [this](const char *key, const QString &value) {
        lua_pushstring(m_state, value.toUtf8().constData());
        lua_setfield(m_state, -2, key);
    };
    field("serial", m_device.serial);
    field("model", m_device.model);
    field("manufacturer", m_device.manufacturer);
    field("android_version", m_device.androidVersion);
    field("sdk", m_device.sdk);
    field("security_patch", m_device.securityPatch);
    lua_setglobal(m_state, "device");
}

void LuaSandbox::resetOutput()
{
    m_html.clear();
    m_actions.clear();
    m_inputs.clear();
    m_statuses.clear();
    m_values.clear();
}

void LuaSandbox::addAction(const QString &id, const QString &label)
{
    if (id.isEmpty())
        return;
    for (auto &action : m_actions) {
        if (action.id == id) {
            action.label = label;
            return;
        }
    }
    m_actions.append({id, label, true});
}

void LuaSandbox::addInput(const InputSpec &input)
{
    if (input.id.isEmpty())
        return;
    for (auto &existing : m_inputs) {
        if (existing.id == input.id) {
            existing = input;
            return;
        }
    }
    m_inputs.append(input);
    if (!m_values.contains(input.id))
        m_values.insert(input.id, input.defaultValue);
}

void LuaSandbox::setStatus(const QString &id, const QString &text)
{
    if (id.isEmpty())
        return;
    for (auto &status : m_statuses) {
        if (status.id == id) {
            status.text = text;
            return;
        }
    }
    m_statuses.append({id, text});
}

void LuaSandbox::setActionEnabled(const QString &id, bool enabled)
{
    for (auto &action : m_actions) {
        if (action.id == id) {
            action.enabled = enabled;
            return;
        }
    }
}

void LuaSandbox::log(const QString &text)
{
    if (m_logger)
        m_logger(text);
}

bool LuaSandbox::run(const QString &script, QString *error)
{
    if (!m_state) {
        if (error)
            *error = QStringLiteral("Lua 状态初始化失败");
        return false;
    }

    resetOutput();
    pushDeviceTable();

    const QByteArray utf8 = script.toUtf8();
    if (luaL_loadstring(m_state, utf8.constData()) != LUA_OK) {
        setError(error, "脚本语法错误");
        return false;
    }
    return protectedCall(0, 0, error);
}

bool LuaSandbox::invokeAction(const QString &id, QString *error)
{
    if (!m_state) {
        if (error)
            *error = QStringLiteral("Lua 状态初始化失败");
        return false;
    }

    lua_getglobal(m_state, "on_action");
    if (!lua_isfunction(m_state, -1)) {
        lua_pop(m_state, 1);
        if (error)
            *error = QStringLiteral("脚本没有定义 on_action(id)");
        return false;
    }

    lua_pushstring(m_state, id.toUtf8().constData());
    return protectedCall(1, 0, error);
}

bool LuaSandbox::protectedCall(int nargs, int nresults, QString *error)
{
    m_elapsed.start();
    lua_sethook(m_state, instructionHook, LUA_MASKCOUNT, 10000);
    const int rc = lua_pcall(m_state, nargs, nresults, 0);
    lua_sethook(m_state, nullptr, 0, 0);

    if (rc != LUA_OK) {
        setError(error, "脚本执行失败");
        return false;
    }
    return true;
}

void LuaSandbox::setError(QString *error, const char *fallback)
{
    const char *message = lua_tostring(m_state, -1);
    if (error)
        *error = QString::fromUtf8(message ? message : fallback);
    lua_pop(m_state, 1);
}

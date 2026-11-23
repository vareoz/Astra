#pragma once

#include "dom/html_parser.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace astra {

struct GcObject;

struct JsValue {
    enum class Kind { Undefined, Null, Bool, Number, String, DomRef, Object };
    Kind kind = Kind::Undefined;
    bool bval = false;
    double nval = 0.0;
    std::string sval;
    DomNode* node = nullptr;
    GcObject* obj = nullptr;

    static JsValue make_undefined() { return {}; }
    static JsValue make_null() { JsValue v; v.kind = Kind::Null; return v; }
    static JsValue make_bool(bool b) { JsValue v; v.kind = Kind::Bool; v.bval = b; return v; }
    static JsValue make_number(double n) { JsValue v; v.kind = Kind::Number; v.nval = n; return v; }
    static JsValue make_string(std::string s) { JsValue v; v.kind = Kind::String; v.sval = std::move(s); return v; }
    static JsValue make_dom(DomNode* n) { JsValue v; v.kind = Kind::DomRef; v.node = n; return v; }
    static JsValue make_obj(GcObject* o) { JsValue v; v.kind = Kind::Object; v.obj = o; return v; }

    std::string to_string() const;
    bool truthy() const;
    double to_number() const;
};

// Bytecode instruction set for AstraJS v2 Stack VM
enum class OpCode : uint8_t {
    PushConst,
    LoadLocal,
    StoreLocal,
    LoadGlobal,
    StoreGlobal,
    Pop,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    Not,
    Eq,
    Neq,
    Lt,
    Le,
    Gt,
    Ge,
    Jump,
    JumpIfFalse,
    MakeArray,
    MakeObject,
    GetProp,
    SetProp,
    Call,
    CallMethod,
    Return,
    Halt
};

struct Instruction {
    OpCode op = OpCode::Halt;
    int32_t operand = 0;
    int32_t operand2 = 0;
};

struct BytecodeChunk {
    std::vector<Instruction> code;
    std::vector<JsValue> constants;
    std::vector<std::string> names;
};

// Mark-and-Sweep GC-managed heap object (Array, Object, Function)
struct GcObject {
    enum class Type { Array, Map, Function };
    Type type = Type::Map;
    bool marked = false;

    std::vector<JsValue> elements;
    std::unordered_map<std::string, JsValue> props;

    // Function fields
    std::string fn_name;
    std::vector<std::string> params;
    BytecodeChunk chunk;
};

struct GcStats {
    size_t live_objects = 0;
    size_t total_allocated = 0;
    size_t total_collected = 0;
    size_t collections = 0;
};

struct JsExecutionReport {
    bool ok = true;
    std::vector<std::string> logs;
    std::string error;
    size_t instructions_executed = 0;
    size_t bytecode_size = 0;
    size_t microtasks_run = 0;
    size_t timers_run = 0;
    GcStats gc;
};

struct JsFetchResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
    bool from_cache = false;
    bool blocked_by_shield = false;
};

// AstraJS v3: Lexer -> Recursive-Descent Compiler -> Stack Bytecode VM
// + Mark-and-Sweep GC + Microtask/Timer Event Loop + DOM Events
// + Web APIs: fetch() (via AstraShield/HttpCache), JSON.parse/stringify, localStorage.
class AstraJsEngine {
public:
    using StorageGetFn = std::function<std::string(const std::string&)>;
    using StorageSetFn = std::function<void(const std::string&, const std::string&)>;
    using FetchFn = std::function<JsFetchResult(const std::string& url)>;

    AstraJsEngine() = default;
    ~AstraJsEngine();

    AstraJsEngine(const AstraJsEngine&) = delete;
    AstraJsEngine& operator=(const AstraJsEngine&) = delete;

    void bind_storage(StorageGetFn getter, StorageSetFn setter);
    void bind_fetch(FetchFn fetcher);
    JsExecutionReport execute(std::string_view code, HtmlDocument& doc);

    // Dispatch a real DOM event (e.g. "click", "input", "submit") to registered JS bytecode handlers
    JsExecutionReport dispatch_event(DomNode* target, const std::string& event_name, HtmlDocument& doc);

    // Trigger explicit Mark-and-Sweep Garbage Collection cycle
    size_t gc_collect();
    GcStats gc_stats() const;

    const std::unordered_map<std::string, JsValue>& globals() const { return globals_; }

private:
    struct TimerEntry {
        int64_t delay_ms = 0;
        JsValue callback;
    };

    GcObject* alloc_object(GcObject::Type type);
    void mark_value(const JsValue& v);
    void mark_object(GcObject* o);

    JsValue run_chunk(const BytecodeChunk& chunk,
                      std::vector<JsValue>& locals,
                      HtmlDocument& doc,
                      JsExecutionReport& rep);
    JsValue invoke_callable(const JsValue& fn,
                            const std::vector<JsValue>& args,
                            HtmlDocument& doc,
                            JsExecutionReport& rep);
    void drain_event_loop(HtmlDocument& doc, JsExecutionReport& rep);

    std::vector<GcObject*> heap_;
    size_t total_allocated_ = 0;
    size_t total_collected_ = 0;
    size_t gc_collections_ = 0;

    std::unordered_map<std::string, JsValue> globals_;
    std::vector<JsValue> stack_;
    std::vector<JsValue> microtasks_;
    std::vector<TimerEntry> timers_;
    // DOM event listeners: (node_ptr, event_name) -> callbacks
    std::map<std::pair<DomNode*, std::string>, std::vector<JsValue>> listeners_;
    std::unordered_map<std::string, std::string> local_storage_;

    StorageGetFn storage_get_;
    StorageSetFn storage_set_;
    FetchFn fetch_fn_;
};

} // namespace astra

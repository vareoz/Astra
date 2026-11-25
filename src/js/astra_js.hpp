#pragma once

#include "dom/html_parser.hpp"
#include "net/websocket_client.hpp"

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

// Bytecode instruction set for AstraJS v7 Stack VM
enum class OpCode : uint8_t {
    PushConst,
    LoadLocal,
    StoreLocal,
    LoadGlobal,
    StoreGlobal,
    LoadThis,
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
    InstanceOf,
    Jump,
    JumpIfFalse,
    MakeArray,
    ArrayPush,
    MakeObject,
    MakeRegExp,
    InheritClass,
    GetProp,
    SetProp,
    Call,
    CallMethod,
    Construct,
    Await,
    MakeClosure,
    PushTry,
    PopTry,
    Throw,
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
    std::vector<std::string> local_names;
};

// Bounded ARGB32 offscreen 2D Canvas backing store (max 1024x1024 = 4 MiB)
struct CanvasSurface {
    static constexpr int kMaxDim = 1024;

    struct PathPoint {
        float x = 0.0f;
        float y = 0.0f;
        bool move_to = false;
    };

    int width = 300;
    int height = 150;
    std::vector<uint32_t> argb;

    uint32_t fill_color = 0xFFFFFFFFu;
    uint32_t stroke_color = 0xFF38BDF8u;
    float line_width = 1.5f;
    float font_size = 13.0f;
    bool font_bold = false;
    bool font_mono = false;
    std::vector<PathPoint> path;

    void resize(int w, int h);
    void blend_pixel(int x, int y, uint32_t c);
    void clear_rect(float x, float y, float w, float h);
    void fill_rect(float x, float y, float w, float h);
    void stroke_rect(float x, float y, float w, float h);
    void begin_path();
    void move_to(float x, float y);
    void line_to(float x, float y);
    void close_path();
    void arc(float cx, float cy, float r, float start_rad, float end_rad);
    void fill_path();
    void stroke_path();
    void fill_text(std::string_view text, float x, float y);

    static uint32_t parse_css_color(std::string_view s, uint32_t fallback);
};

// Mark-and-Sweep GC-managed heap object (Array, Map, Function, Class, Promise, Canvas2D, StyleProxy, WebSocket, RegExp, Date, JsMap, JsSet)
struct GcObject {
    enum class Type { Array, Map, Function, Class, Promise, Canvas2D, StyleProxy, WebSocket,
                      RegExp, Date, JsMap, JsSet, Event };
    enum class PromiseState : uint8_t { Pending, Fulfilled, Rejected };
    struct PromiseReaction {
        GcObject* next = nullptr;
        JsValue on_fulfilled;
        JsValue on_rejected;
        bool is_finally = false;
    };

    Type type = Type::Map;
    bool marked = false;

    std::vector<JsValue> elements;
    std::unordered_map<std::string, JsValue> props;
    GcObject* proto_class = nullptr;
    GcObject* proto_obj = nullptr;      // Real [[Prototype]] chain link
    GcObject* fn_prototype = nullptr;   // Constructor .prototype object

    // Function / Class fields
    std::string fn_name;
    std::vector<std::string> params;
    BytecodeChunk chunk;
    bool is_async = false;
    std::shared_ptr<std::unordered_map<std::string, JsValue>> closure_env;

    // RegExp / Date / ES6 Map / ES6 Set native storage
    std::string regex_pattern;
    std::string regex_flags;
    int64_t date_ms = 0;
    std::vector<std::pair<JsValue, JsValue>> map_entries;
    std::vector<JsValue> set_entries;

    // DOM proxies & native bindings
    DomNode* classlist_node = nullptr;
    DomNode* style_node = nullptr;
    std::string canvas_id;
    std::shared_ptr<WebSocketClient> ws_client;

    // Promise fields
    PromiseState promise_state = PromiseState::Pending;
    JsValue promise_value;
    std::vector<PromiseReaction> reactions;
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
    size_t raf_run = 0;
    GcStats gc;
    // Session 8: resultado del despacho de eventos DOM.
    bool default_prevented = false;
    size_t listeners_invoked = 0;
};

struct JsFetchResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
    bool from_cache = false;
    bool blocked_by_shield = false;
};

// AstraJS v6: Lexer -> Recursive-Descent Compiler -> Stack Bytecode VM
// + try/catch/throw stack unwinding + for..of + async/await + lexical closures
// + Mark-and-Sweep GC + Microtask/Timer/requestAnimationFrame Event Loop
// + HTML5 <canvas> 2D API (bounded ARGB32 CanvasSurface) + Real Promises
// + DOM Events/innerHTML/classList + fetch() + JSON + localStorage + MV3 storage.
class AstraJsEngine {
public:
    using StorageGetFn = std::function<std::string(const std::string&)>;
    using StorageSetFn = std::function<void(const std::string&, const std::string&)>;
    using FetchFn = std::function<JsFetchResult(const std::string& url,
                                                const std::string& method,
                                                const std::string& body,
                                                const std::vector<std::pair<std::string, std::string>>& headers)>;

    using HistoryFn = std::function<void(const std::string& new_url, bool replace)>;

    AstraJsEngine() = default;
    ~AstraJsEngine();

    AstraJsEngine(const AstraJsEngine&) = delete;
    AstraJsEngine& operator=(const AstraJsEngine&) = delete;

    // Session 8: `document.cookie` se delega al tarro particionado de
    // AstraShield. El getter devuelve la cabecera Cookie visible para el
    // origen activo y el setter procesa una cadena Set-Cookie, de modo que la
    // Total Cookie Protection sigue aplicándose a lo que escribe el script.
    using CookieGetFn = std::function<std::string()>;
    using CookieSetFn = std::function<void(const std::string&)>;

    void bind_storage(StorageGetFn getter, StorageSetFn setter);
    void bind_fetch(FetchFn fetcher);
    void bind_history(HistoryFn on_history, std::string initial_url);
    const std::string& current_url() const { return current_url_; }
    void bind_cookies(CookieGetFn getter, CookieSetFn setter);
    JsExecutionReport execute(std::string_view code, HtmlDocument& doc);

    // Dispatch a real DOM event (e.g. "click", "input", "submit") to registered JS bytecode handlers
    // Despacha un evento con las tres fases de DOM Events Level 2 (captura,
    // objetivo y burbujeo) construyendo un objeto Event real con `target`,
    // `currentTarget`, `eventPhase`, `stopPropagation()`,
    // `stopImmediatePropagation()` y `preventDefault()`.
    JsExecutionReport dispatch_event(DomNode* target, const std::string& event_name, HtmlDocument& doc,
                                     bool bubbles = true, bool cancelable = true);

    // Execute one queued requestAnimationFrame batch (callbacks may re-enqueue for the next frame)
    JsExecutionReport run_animation_frame(double timestamp_ms, HtmlDocument& doc);
    size_t pending_raf_count() const { return raf_callbacks_.size(); }

    // Lookup offscreen <canvas> surface by element id (or "canvas" fallback)
    const CanvasSurface* find_canvas(const std::string& id) const;

    // Trigger explicit Mark-and-Sweep Garbage Collection cycle
    size_t gc_collect();
    GcStats gc_stats() const;

    const std::unordered_map<std::string, JsValue>& globals() const { return globals_; }

private:
    struct TimerEntry {
        int64_t delay_ms = 0;
        JsValue callback;
    };

    struct MicrotaskEntry {
        JsValue callback;
        JsValue arg;
        bool has_arg = false;
        GcObject* target_promise = nullptr;
        bool passthrough_reject = false;
        bool is_finally = false;
        bool source_rejected = false;
    };

    GcObject* alloc_object(GcObject::Type type);
    void mark_value(const JsValue& v);
    void mark_object(GcObject* o);

    GcObject* make_resolved_promise(const JsValue& val);
    GcObject* make_rejected_promise(const JsValue& reason);
    void settle_promise(GcObject* p, bool rejected, const JsValue& val);
    void enqueue_reaction(GcObject* p, const GcObject::PromiseReaction& r);
    GcObject* chain_promise(GcObject* p, const JsValue& on_fulfilled, const JsValue& on_rejected, bool is_finally = false);

    JsValue run_chunk(const BytecodeChunk& chunk,
                      std::vector<JsValue>& locals,
                      HtmlDocument& doc,
                      JsExecutionReport& rep,
                      const std::shared_ptr<std::unordered_map<std::string, JsValue>>& closure_env = nullptr,
                      const JsValue& this_val = JsValue::make_undefined());
    JsValue invoke_callable(const JsValue& fn,
                            const std::vector<JsValue>& args,
                            HtmlDocument& doc,
                            JsExecutionReport& rep,
                            const JsValue& this_val = JsValue::make_undefined());
    void drain_microtasks_only(HtmlDocument& doc, JsExecutionReport& rep);
    void drain_event_loop(HtmlDocument& doc, JsExecutionReport& rep);

    std::vector<GcObject*> heap_;
    size_t total_allocated_ = 0;
    size_t total_collected_ = 0;
    size_t gc_collections_ = 0;

    std::unordered_map<std::string, JsValue> globals_;
    std::vector<JsValue> stack_;
    std::vector<MicrotaskEntry> microtasks_;
    std::vector<TimerEntry> timers_;
    std::vector<JsValue> raf_callbacks_;
    uint32_t raf_id_counter_ = 0;

    bool exception_active_ = false;
    JsValue exception_value_;

    std::unordered_map<std::string, CanvasSurface> canvases_;
    // DOM event listeners: (node_ptr, event_name) -> callbacks
    // Session 8 — DOM Events Level 2: cada listener recuerda si se registró
    // para la fase de captura, porque eso decide en qué recorrido del árbol se
    // ejecuta (descendente para captura, ascendente para burbujeo).
    struct EventListener {
        JsValue callback;
        bool capture = false;
        bool once = false;
    };
    std::map<std::pair<DomNode*, std::string>, std::vector<EventListener>> listeners_;
    CookieGetFn cookie_get_;
    CookieSetFn cookie_set_;
    std::unordered_map<std::string, std::string> local_storage_;
    std::unordered_map<std::string, std::string> session_storage_;
    std::string current_url_ = "about:blank";
    std::vector<std::pair<std::string, JsValue>> history_entries_;
    int history_index_ = -1;

    StorageGetFn storage_get_;
    StorageSetFn storage_set_;
    FetchFn fetch_fn_;
    HistoryFn history_fn_;
};

} // namespace astra

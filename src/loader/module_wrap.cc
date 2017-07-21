#include <algorithm>
#include "module_wrap.h"

#include "../env.h"
#include "../node_url.h"
#include "util.h"
#include "util-inl.h"

namespace node {
namespace loader {
using namespace node::url;

using v8::Local;
using v8::Persistent;
using v8::MaybeLocal;
using v8::Promise;
using v8::Object;
using v8::Module;
using v8::Value;
using v8::Isolate;
using v8::String;
using v8::FunctionCallbackInfo;
using v8::Context;
using v8::JSON;
using v8::PropertyCallbackInfo;
using v8::Function;
using v8::IntegrityLevel;
using v8::ScriptCompiler;
using v8::ScriptOrigin;
using v8::Integer;
using v8::FunctionTemplate;
using v8::EscapableHandleScope;
using v8::PropertyDescriptor;

const std::string EXTENSIONS[] = {".mjs", ".js", ".json", ".node"};
std::map<int, std::vector<ModuleWrap*>*> ModuleWrap::module_map_;

ModuleWrap::ModuleWrap(Environment* env,
                      Local<Object> object,
                      Local<Module> module,
                      Local<String> url
                     ) : BaseObject(env, object) {
  Isolate* iso = Isolate::GetCurrent();
  module_.Reset(iso, module);
  url_.Reset(iso, url);
}

ModuleWrap::~ModuleWrap() {
  auto module = module_.Get(Isolate::GetCurrent());
  auto same_hash = module_map_[module->GetIdentityHash()];
  auto it = std::find(same_hash->begin(), same_hash->end(), this);
  if (it != same_hash->end()) {
    same_hash->erase(it);
  }
  module_.Reset();
}

void ModuleWrap::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  Isolate* iso = args.GetIsolate();
  if (!args.IsConstructCall()) {
    env->ThrowError("constructor must be called using new");
    return;
  }
  if (args.Length() != 2) {
    env->ThrowError("constructor must have exactly 2 argument (string, string)");
    return;
  }

  if (!args[0]->IsString()) {
    env->ThrowError("first argument is not a string");
    return;
  }
  auto arg = args[0]->ToString();
  String::Utf8Value source_text(arg);

  if (!args[1]->IsString()) {
    env->ThrowError("second argument is not a string");
    return;
  }
  Local<String> url = args[1]->ToString();

  Local<Module> mod;
  // compile
  {
    ScriptOrigin origin(url,
                            Integer::New(iso, 0),
                            Integer::New(iso, 0),
                            False(iso),
                            Integer::New(iso, 0),
                            String::NewFromUtf8(iso, ""),
                            False(iso),
                            False(iso),
                            True(iso));
    ScriptCompiler::Source source(arg, origin);
    auto maybe_mod = ScriptCompiler::CompileModule(iso, &source);
    if (maybe_mod.IsEmpty()) {
        return;
    }
    mod = maybe_mod.ToLocalChecked();
  }
  auto that = args.This();
  auto ctx = that->CreationContext();
  if (!that->Set(ctx, FIXED_ONE_BYTE_STRING(iso, "url"), url).FromJust(false)) {
    return;
  }
  ModuleWrap* obj =
      new ModuleWrap(Environment::GetCurrent(ctx), that, mod, url);
  if (ModuleWrap::module_map_.count(mod->GetIdentityHash()) == 0) {
    ModuleWrap::module_map_[mod->GetIdentityHash()] =
        new std::vector<ModuleWrap*>();
  }
  ModuleWrap::module_map_[mod->GetIdentityHash()]->push_back(obj);
  Wrap(that, obj);

  that->SetIntegrityLevel(ctx, IntegrityLevel::kFrozen);
  args.GetReturnValue().Set(that);
}

void ModuleWrap::Link(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate* iso = args.GetIsolate();
  EscapableHandleScope handle_scope(iso);
  if (!args[0]->IsFunction()) {
    env->ThrowError("first argument is not a function");
    return;
  }

  Local<Function> resolverArg = args[0].As<Function>();

  auto that = args.This();
  ModuleWrap* obj = Unwrap<ModuleWrap>(that);
  if (obj->linked_) return;
  obj->linked_ = true;
  Local<Module> mod(obj->module_.Get(iso));

  // call the dependency resolve callbacks
  for (auto i = 0; i < mod->GetModuleRequestsLength(); i++) {
    auto specifier = mod->GetModuleRequest(i);
    String::Utf8Value utf8_specifier(specifier);
    std::string std_specifier = *utf8_specifier;

    Local<Value> argv[] = {
      specifier
    };

    MaybeLocal<Value> maybeResolveReturnValue = resolverArg->Call(that->CreationContext(), that, 1, argv);
    if (maybeResolveReturnValue.IsEmpty()) {
      return;
    }
    Local<Value> resolveReturnValue = maybeResolveReturnValue.ToLocalChecked();
    Local<Promise> resolvePromise = Local<Promise>::Cast(resolveReturnValue);
    obj->resolve_cache_[std_specifier] = new Persistent<Promise>();
    obj->resolve_cache_[std_specifier]->Reset(iso, resolvePromise);
  }

  args.GetReturnValue().Set(handle_scope.Escape(that));
}

void ModuleWrap::Instantiate(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  auto iso = args.GetIsolate();
  auto that = args.This();
  auto ctx = that->CreationContext();

  ModuleWrap* obj = Unwrap<ModuleWrap>(that);
  bool ok = obj->module_.Get(iso)->Instantiate(ctx, ModuleWrap::ResolveCallback);

  // clear resolve cache on instantiate
  obj->resolve_cache_.clear();

  if (!ok) {
    return;
  }
}

void ModuleWrap::Evaluate(const FunctionCallbackInfo<Value>& args) {
  auto iso = args.GetIsolate();
  auto that = args.This();
  auto ctx = that->CreationContext();
  ModuleWrap* obj = Unwrap<ModuleWrap>(that);
  auto result = obj->module_.Get(iso)->Evaluate(ctx);
  if (result.IsEmpty()) {
    return;
  }
  auto ret = result.ToLocalChecked();
  args.GetReturnValue().Set(ret);
}

MaybeLocal<Module> ModuleWrap::ResolveCallback(Local<Context> context,
                                                       Local<String> specifier,
                                                       Local<Module> referrer) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* iso = Isolate::GetCurrent();
  if (ModuleWrap::module_map_.count(referrer->GetIdentityHash()) == 0) {
    env->ThrowError("linking error, unknown module");
    return MaybeLocal<Module>();
  }

  auto possible_deps = ModuleWrap::module_map_[referrer->GetIdentityHash()];
  ModuleWrap* dependent = nullptr;

  for (auto possible_dep : *possible_deps) {
    if (possible_dep->module_ == referrer) {
      dependent = possible_dep;
    }
  }

  String::Utf8Value ss(specifier);

  if (dependent == nullptr) {
    env->ThrowError("linking error, null dep");
    return MaybeLocal<Module>();
  }

  String::Utf8Value utf8_specifier(specifier);
  std::string std_specifier = *utf8_specifier;

  if (dependent->resolve_cache_.count(std_specifier) != 1) {
    env->ThrowError("linking error, not in local cache");
    return MaybeLocal<Module>();
  }

  Local<Promise> resolvePromise = dependent->resolve_cache_[std_specifier]->Get(iso);

  if (resolvePromise->State() != Promise::kFulfilled) {
    env->ThrowError("linking error, dependency promises must be resolved on instantiate");
    return MaybeLocal<Module>();
  }

  MaybeLocal<Object> moduleObject = resolvePromise->Result()->ToObject();

  if (moduleObject.IsEmpty()) {
    env->ThrowError("linking error, expected a valid module object from resolver");
    return MaybeLocal<Module>();
  }

  ModuleWrap* mod = Unwrap<ModuleWrap>(moduleObject.ToLocalChecked());

  return mod->module_.Get(context->GetIsolate());
}

namespace {
  URL __init_cwd() {
    std::string specifier = "file://";
    #ifdef _WIN32
      /* MAX_PATH is in characters, not bytes. Make sure we have enough headroom. */
      char buf[MAX_PATH * 4];
    #else
      char buf[PATH_MAX];
    #endif

    size_t cwd_len = sizeof(buf);
    int err = uv_cwd(buf, &cwd_len);
    if (err) {
      return URL("");
    }
    specifier += buf;
    specifier += "/";
    return URL(specifier);
  }
  static URL INITIAL_CWD(__init_cwd());
  inline bool is_relative_or_absolute_path(std::string specifier) {
    auto len = specifier.length();
    if (len <= 0) return false;
    else if (specifier[0] == '/') return true;
    else if (specifier[0] == '.') {
      if (len == 1 || specifier[1] == '/') return true;
      else if (specifier[1] == '.') {
        if (len == 2 || specifier[2] == '/') return true;
      }
    }
    return false;
  }
  struct read_result {
    bool had_error = false;
    std::string source;
  } read_result;
  inline const struct read_result read_file(uv_file file) {
    struct read_result ret;
    std::string src;
    uv_fs_t req;
    void* base = malloc(4096);
    if (base == nullptr) {
      ret.had_error = true;
      return ret;
    }
    uv_buf_t buf = uv_buf_init(static_cast<char*>(base), 4096);
    uv_fs_read(uv_default_loop(), &req, file, &buf, 1, 0, nullptr);
    while (req.result > 0) {
      src += std::string(static_cast<const char*>(buf.base), req.result);
      uv_fs_read(uv_default_loop(), &req, file, &buf, 1, src.length(), nullptr);
    }
    ret.source = src;
    return ret;
  }
  struct file_check {
    bool failed = true;
    uv_file file;
  } file_check;
  inline const struct file_check check_file(URL search, bool close = false, bool allow_dir = false) {
    struct file_check ret;
    uv_fs_t fs_req;
    uv_fs_open(nullptr, &fs_req, search.path().c_str(), O_RDONLY, 0, nullptr);
    auto fd = fs_req.result;
    if (fd < 0) {
      return ret;
    }
    if (!allow_dir) {
      uv_fs_fstat(nullptr, &fs_req, fd, nullptr);
      if (S_ISDIR(fs_req.statbuf.st_mode)) {
        uv_fs_close(nullptr, &fs_req, fd, nullptr);
        return ret;
      }
    }
    ret.failed = false;
    ret.file = fd;
    if (close) uv_fs_close(nullptr, &fs_req, fd, nullptr);
    return ret;
  }
  URL resolve_extensions(URL search, bool check_exact = true) {
    if (check_exact) {
      auto check = check_file(search, true);
      if (!check.failed) {
        return search;
      }
    }
    for (auto extension : EXTENSIONS) {
      auto guess = URL(search.path() + extension, &search);
      auto check = check_file(guess, true);
      if (!check.failed) {
        return guess;
      }
    }
    return URL("");
  }
  inline URL resolve_index(URL search) {
    return resolve_extensions(URL("index", &search), false);
  }
  URL resolve_main(URL search) {
    URL pkg = URL("package.json", &search);
    auto check = check_file(pkg);
    if (!check.failed) {
      auto iso = Isolate::GetCurrent();
      auto read = read_file(check.file);
      uv_fs_t fs_req;
      // if we fail to close :-/
      uv_fs_close(nullptr, &fs_req, check.file, nullptr);
      if (read.had_error) return URL("");
      auto pkg_src = read.source.c_str();
      auto src = String::NewFromUtf8(iso, pkg_src, String::kNormalString);
      MaybeLocal<Value> pkg_json = JSON::Parse(iso->GetCurrentContext(), src);
      if (pkg_json.IsEmpty()) return URL("");
      Local<String> pkg_main = pkg_json.ToLocalChecked()->ToObject()->Get(String::NewFromUtf8(iso, "main"))->ToString();
      String::Utf8Value main_utf8(pkg_main);
      std::string main_std(*main_utf8);
      if (!is_relative_or_absolute_path(main_std)) {
        main_std.insert(0, "./");
      }
      URL main = Resolve(main_std, &search);
      return main;
    }
    return URL("");
  }
  URL resolve_module(std::string specifier, URL* base) {
    URL parent = *base;
    URL dir("");
    do {
      dir = parent;
      auto check = Resolve("./node_modules/" + specifier, &dir, true);
      if (!(check.flags() & URL_FLAGS_FAILED)) {
        const auto limit = specifier.find('/');
        std::string chroot = dir.path() + "node_modules/" + specifier.substr(
          0,
          limit == std::string::npos ? specifier.length() : limit + 1
        );
        if (check.path().substr(0, chroot.length()) != chroot) {
          // fprintf(stderr, "tried to escape root\n");
          return URL("");
        }
        return check;
      }
      else {
        // TODO, PREVENT FALLTHROUGH
      }
      parent = URL("..", &dir);
    } while (parent.path() != dir.path());
    return URL("");
  }
} // anonymous namespace

URL Resolve(std::string specifier, URL* base, bool read_pkg_json) {
  auto pure_url = URL(specifier.c_str());
  // printf("resolving, %s against %s\n", specifier.c_str(), base->path().c_str());
  if (!(pure_url.flags() & URL_FLAGS_FAILED)) {
    return pure_url;
  }
  if (specifier.length() == 0) {
    return URL("");
  }
  if (is_relative_or_absolute_path(specifier)) {
    auto resolved = URL(specifier, base);
    auto file = resolve_extensions(resolved);
    if (!(file.flags() & URL_FLAGS_FAILED)) return file;
    if (specifier.back() != '/') {
      resolved = URL(specifier + "/", base);
    }
    if (read_pkg_json) {
      auto main = resolve_main(resolved);
      if (!(main.flags() & URL_FLAGS_FAILED)) return main;
    }
    return resolve_index(resolved);
  }
  else {
    return resolve_module(specifier, base);
  }
  return URL("");
}

void ModuleWrap::Resolve(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  if (args.IsConstructCall()) {
    env->ThrowError("resolve() must not be called as a constructor");
    return;
  }
  if (args.Length() != 2) {
    env->ThrowError("resolve must have exactly 2 arguments (string, string)");
    return;
  }

  if (!args[0]->IsString()) {
    env->ThrowError("first argument is not a string");
    return;
  }
  auto specifier_arg = args[0]->ToString();
  String::Utf8Value specifier_utf(specifier_arg);

  if (!args[1]->IsString()) {
    env->ThrowError("second argument is not a string");
    return;
  }
  auto url_arg = args[1]->ToString();
  String::Utf8Value url_utf(url_arg);
  URL url(*url_utf);
  // printf("resolving, args.Length() %d %s\n", args.Length(), *url_utf);
  if (url.flags() & URL_FLAGS_FAILED) {
    env->ThrowError("second argument is not a URL string");
    return;
  }

  URL result = node::loader::Resolve(*specifier_utf, &url, true);
  if (result.flags() & URL_FLAGS_FAILED) {
    env->ThrowError("module not found");
    return;
  }

  const Local<Value> ret(result.ToObject(
    node::Environment::GetCurrent(args)));
  args.GetReturnValue().Set(ret);
}

void ModuleWrap::Initialize(Local<Object> target,
                           Local<Value> unused,
                           Local<Context> context) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();

  Local<FunctionTemplate> tpl = env->NewFunctionTemplate(New);
  tpl->SetClassName(FIXED_ONE_BYTE_STRING(isolate, "ModuleWrap"));
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  env->SetProtoMethod(tpl, "link", Link);
  env->SetProtoMethod(tpl, "instantiate", Instantiate);
  env->SetProtoMethod(tpl, "evaluate", Evaluate);

  target->Set(FIXED_ONE_BYTE_STRING(isolate, "ModuleWrap"), tpl->GetFunction());
  env->SetMethod(target, "resolve", node::loader::ModuleWrap::Resolve);
}

} // namespace loader
} // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(module_wrap, node::loader::ModuleWrap::Initialize)

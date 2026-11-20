#!/usr/bin/env python3
"""Generate the host side of the guest module's imports: ios_imports.c.

wasm2c declares each import as w2c_env_<name>(struct w2c_env *, <wasm
types>) (the module's header, guest.h). Its meaning is in the guest's C
declarations, the same the Android port uses:

- port/android/guest/runtime/guest_host.h: the host services (host_*);
- the Android graph's generated guest_gl.c: hostgl_<GL function>, whose
  pointer-sized GL integers the guest already widened to long long;
- the Android graph's generated guest_posix.c: hostposix_<posix.h name>.

Each wrapper turns the guest's 32-bit addresses into host pointers (offsets
into the guest's linear memory, NULL staying NULL) and calls

- host_<name> for a host service (ios/host, or a stub below);
- the OpenGL ES function itself for hostgl_* (Apple's OpenGL ES 3.0; the
  few functions it lacks are stubs the renderer never reaches, as it checks
  the context's version and extensions first);
- posix_<name> for the file helpers of port/linux/src/posix_files.c, which
  the host compiles; the rest of posix.h (sockets, DTLS, UPnP, Discord, MCC
  sharing) is a stub that fails, for now.

Usage: gen_imports.py guest.h guest_host.h guest_gl.c guest_posix.c output.c
"""

import re
import sys

# host services ios/host implements with the wasm types themselves
RAW_HOST = {
    "host_syscall", "host_get_tp", "host_set_tp", "host_thread_create",
}

# host services not offered on iOS: what each returns
HOST_STUBS = {
    "host_mcc_memory_reserve": "-38",  # -ENOSYS
    "host_mcc_memory_release": "-38",
    "host_mcc_hash_start": "-1",
    "host_mcc_hash_wait": "-1",
    "host_mcc_hash_cancel": None,
    "host_sha256_start": "-1",
    "host_sha256_poll": "-1",
    "host_sha256_cancel": None,
    "host_https_get": "0",
}
STEAM_STUB_RESULT = "0"

# GL "pointers" that are offsets into the bound buffer: (function, index)
GL_OFFSETS = {
    ("glVertexAttribPointer", 5),
    ("glVertexAttribIPointer", 4),
    ("glDrawElements", 3),
    ("glDrawElementsBaseVertex", 3),
    ("glDrawElementsBaseVertexEXT", 3),
    ("glDrawElementsBaseVertexOES", 3),
}
# not in Apple's OpenGL ES 3.0
GL_MISSING = {
    "glCopyImageSubData", "glMemoryBarrier", "glDrawElementsBaseVertex",
    "glDrawElementsBaseVertexEXT", "glDrawElementsBaseVertexOES",
}

# posix.h functions the host implements (port/linux/src/posix_files.c), and
# the few ios/host/host_misc.c gives
POSIX_NATIVE = {
    "stat", "fstat", "set_file_times", "seek", "truncate", "disk_space",
    "set_read_only", "make_directory", "directory_open", "directory_next",
    "directory_close", "find_entry_case_insensitive",
}
POSIX_HOST = {
    "random_bytes", "process_id", "command_line_argument",
    "local_ipv4_address", "resolve_ipv4", "register_url_scheme",
}
# posix.h parameters that are the host's own handles, not guest addresses
POSIX_HANDLES = {("directory_next", 0), ("directory_close", 0)}


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def parse_wasm_imports(header):
    """name -> (result wasm type or 'void', [param wasm types])"""
    imports = {}
    for match in re.finditer(r"^(\w+) w2c_env_(\w+)\(struct w2c_env\*(?:, ([^)]*))?\);", header, re.M):
        result, name, params = match.group(1), match.group(2), match.group(3)
        imports[name] = (result, [p.strip() for p in params.split(",")] if params else [])
    return imports


def split_params(params):
    params = " ".join(params.split())
    if params in ("", "void"):
        return []
    return [p.strip() for p in params.split(",")]


def param_type(param):
    """the type of a declaration's parameter, without its name"""
    param = re.sub(r"\b(restrict|__restrict)\b", "", param).strip()
    if param.endswith("*"):
        return param
    match = re.match(r"^(.*?[\s\*])(\w+)$", param)
    if match and match.group(1).strip() and match.group(2) not in (
            "int", "char", "short", "long", "float", "double", "GLenum", "GLuint", "GLint", "GLsizei",
            "GLboolean", "GLbitfield", "GLfloat", "GLubyte", "GLchar", "GLintptr", "GLsizeiptr", "GLclampf",
            "posix_ulong", "posix_long", "unsigned", "signed"):
        return match.group(1).strip()
    return param


def parse_c_prototypes(text, prefix):
    text = strip_comments(text)
    result = {}
    for match in re.finditer(r"^([A-Za-z_][\w \*]*?)\s*\b(" + prefix + r"\w+)\s*\(([^;{]*?)\)\s*(?:__attribute__\(\([^;]*\)\))?\s*;",
                             text, re.M | re.S):
        ret = " ".join(match.group(1).split())
        name = match.group(2)
        result[name] = (ret, [param_type(p) for p in split_params(match.group(3))])
    return result


def is_pointer(c_type):
    return "*" in c_type


def convert(c_type, value, raw=False):
    # (the Android guest widens a pointer passed on the AArch64 stack, past
    # the eighth integer argument, to unsigned long long: still an address)
    if c_type == "unsigned long long":
        return f"(const void *)GP((uint32_t){value})"
    if is_pointer(c_type):
        if raw:
            return f"({c_type})(uintptr_t){value}"
        return f"({c_type})GP({value})"
    return f"({c_type}){value}"


def wasm_param_list(params):
    return "".join(f", {t} a{i}" for i, t in enumerate(params))


def result_cast(wasm_result, c_ret, expression):
    if wasm_result == "void":
        return f"\t{expression};"
    if is_pointer(c_ret):
        return f"\treturn ({wasm_result})(uintptr_t){expression};"
    return f"\treturn ({wasm_result}){expression};"


def main():
    header_path, host_h, gl_c, posix_c, output = sys.argv[1:6]
    wasm = parse_wasm_imports(open(header_path).read())
    host = parse_c_prototypes(open(host_h).read(), "host_")
    gl = parse_c_prototypes(open(gl_c).read(), "hostgl_")
    posix = parse_c_prototypes(open(posix_c).read(), "hostposix_")

    out = [
        "/* generated by ios/tools/gen_imports.py; do not edit */",
        "#include \"host.h\"",
        "#include \"guest.h\"",
        "#include \"posix.h\"",
        "#include <OpenGLES/ES3/gl.h>",
        "#include <OpenGLES/ES3/glext.h>",
        "#include <stdint.h>",
        "",
        "/* the Linux errno of the last host call that failed on this thread */",
        "",
    ]
    declarations = []
    bodies = []
    missing = []

    for name in sorted(wasm):
        result, params = wasm[name]
        signature = f"{result} w2c_env_{name}(struct w2c_env *env{wasm_param_list(params)})"
        if name == "memory":
            continue
        if name in ("_ReadWriteBarrier", "0x5FReadWriteBarrier"):
            bodies += [signature, "{", "\t(void)env;", "\t__atomic_thread_fence(__ATOMIC_SEQ_CST);", "}", ""]
            continue
        if name in RAW_HOST:
            continue
        if name.startswith("host_steam_") or name in HOST_STUBS:
            value = HOST_STUBS.get(name, STEAM_STUB_RESULT)
            body = [signature, "{", "\t(void)env;"]
            body += [f"\t(void)a{i};" for i in range(len(params))]
            if result != "void":
                body.append(f"\treturn ({result}){value or 0};")
            bodies += body + ["}", ""]
            continue
        if name.startswith("hostgl_"):
            function = name[len("hostgl_"):]
            ret, c_params = gl[name]
            if function in GL_MISSING:
                body = [signature, "{", "\t(void)env;"]
                body += [f"\t(void)a{i};" for i in range(len(params))]
                body.append(f"\thost_gl_missing(\"{function}\");")
                if result != "void":
                    body.append(f"\treturn 0;")
                bodies += body + ["}", ""]
                continue
            if function == "glShaderSource":
                bodies += [signature, "{",
                           "\t/* the guest passes the strings' addresses widened to 64 bits */",
                           "\tconst uint64_t *wide = (const uint64_t *)GP(a2);",
                           "\tconst GLchar *strings[64];",
                           "\tGLsizei index, count = (GLsizei)a1 < 64 ? (GLsizei)a1 : 64;",
                           "\t(void)env;",
                           "\tfor (index = 0; index < count; index++)",
                           "\t\tstrings[index] = (const GLchar *)GP((uint32_t)wide[index]);",
                           "\tglShaderSource((GLuint)a0, count, strings, (const GLint *)GP(a3));",
                           "}", ""]
                continue
            args = [convert(c_params[i], f"a{i}", (function, i) in GL_OFFSETS) for i in range(len(params))]
            bodies += [signature, "{", "\t(void)env;", result_cast(result, ret, f"{function}({', '.join(args)})"), "}", ""]
            continue
        if name.startswith("hostposix_"):
            function = name[len("hostposix_"):]
            ret, c_params = posix[name]
            if function in POSIX_NATIVE or function in POSIX_HOST:
                target = f"posix_{function}" if function in POSIX_NATIVE else f"host_posix_{function}"
                args = [convert(c_params[i], f"a{i}", (function, i) in POSIX_HANDLES) for i in range(len(params))]
                if function in POSIX_HOST:
                    declarations.append(f"{ret} {target}({', '.join(c_params) or 'void'});")
                bodies += [signature, "{", "\t(void)env;", result_cast(result, ret, f"{target}({', '.join(args)})"), "}", ""]
            else:
                body = [signature, "{", "\t(void)env;"]
                body += [f"\t(void)a{i};" for i in range(len(params))]
                body.append("\thost_set_errno_nosys();")
                if result != "void":
                    body.append(f"\treturn ({result})-1;")
                bodies += body + ["}", ""]
            continue
        if name.startswith("host_"):
            if name not in host:
                missing.append(name)
                continue
            ret, c_params = host[name]
            args = [convert(c_params[i], f"a{i}") for i in range(len(params))]
            bodies += [signature, "{", "\t(void)env;", result_cast(result, ret, f"{name}({', '.join(args)})"), "}", ""]
            continue
        missing.append(name)

    if missing:
        raise SystemExit("imports with no host side: " + ", ".join(missing))
    # the host services' own declarations, as the guest sees them, with host types
    for name in sorted(host):
        if name in RAW_HOST or name.startswith("host_steam_") or name in HOST_STUBS:
            continue
        ret, c_params = host[name]
        declarations.append(f"{ret} {name}({', '.join(c_params) or 'void'});")
    with open(output, "w") as f:
        f.write("\n".join(out + declarations + ["", "void host_gl_missing(const char *name);",
                                                "void host_set_errno_nosys(void);", ""] + bodies))


if __name__ == "__main__":
    main()

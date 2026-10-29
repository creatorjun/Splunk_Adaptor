# tests/architecture_test.py
import io
import pathlib
import posixpath
import re
import sys
import tokenize


def has_cpp_comment(text):
    index = 0
    while index < len(text):
        if text.startswith('R"', index):
            opening = text.find('(', index + 2)
            if opening == -1:
                raise AssertionError('Unterminated raw string')
            ending = ')' + text[index + 2:opening] + '"'
            closing = text.find(ending, opening + 1)
            if closing == -1:
                raise AssertionError('Unterminated raw string')
            index = closing + len(ending)
        elif text[index] in ('"', "'"):
            quote = text[index]
            index += 1
            while index < len(text):
                if text[index] == '\\':
                    index += 2
                elif text[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
        elif text.startswith('//', index) or text.startswith('/*', index):
            return True
        else:
            index += 1
    return False


INNER_HEADERS = set('''algorithm any array atomic bit bitset cassert cctype cerrno charconv
chrono cfloat cinttypes climits clocale cmath compare complex concepts cstddef cstdint
cstring cwchar cwctype deque exception execution expected functional initializer_list
iterator limits list map memory memory_resource new numbers numeric optional queue ranges
ratio regex scoped_allocator set source_location span stack stdexcept stop_token string
string_view system_error tuple type_traits typeindex typeinfo unordered_map unordered_set
utility valarray variant vector version'''.split())
LAYERS = {'domain', 'application', 'infrastructure', 'runtime'}
DEPENDENCIES = {
    'domain': {'domain'},
    'application': {'domain', 'application'},
    'infrastructure': {'domain', 'application', 'infrastructure'},
    'runtime': {'domain', 'application', 'runtime'},
}


def project_header(relative, header, quoted):
    if header.startswith('src/'):
        return posixpath.normpath(header)
    if header.split('/', 1)[0] in LAYERS:
        return posixpath.normpath('src/' + header)
    if header.startswith('.') or (quoted and '/' not in header):
        return posixpath.normpath(posixpath.join(posixpath.dirname(relative), header))
    return None


def check_includes(relative, text):
    parts = pathlib.PurePosixPath(relative).parts
    if len(parts) < 3 or parts[0] != 'src' or parts[1] not in LAYERS:
        return
    layer = parts[1]
    includes = re.findall(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', text, re.MULTILINE)
    for delimiter, raw_header in includes:
        header = raw_header.replace('\\', '/')
        assert not header.startswith('/') and not re.match(r'^[A-Za-z]:', header), (relative, header)
        resolved = project_header(relative, header, delimiter == '"')
        if resolved is not None:
            target = pathlib.PurePosixPath(resolved).parts
            assert len(target) >= 3 and target[0] == 'src' and target[1] in LAYERS, (relative, header)
            if layer == 'runtime' and resolved == 'src/infrastructure/linux/UniqueFd.hpp':
                continue
            assert target[1] in DEPENDENCIES[layer], (relative, header)
        elif layer in ('domain', 'application'):
            assert header in INNER_HEADERS, (relative, header)
        elif layer == 'runtime':
            assert not header.startswith(('nlohmann/', 'mysql/', 'mariadb/')), (relative, header)
            assert header != 'mysql.h', (relative, header)


def check_build_dependencies(text):
    allowed = {
        'monitor_domain': {'monitor_options'},
        'monitor_application': {'monitor_domain', 'monitor_options'},
        'monitor_infrastructure': {'monitor_application', 'monitor_options', 'nlohmann_json::nlohmann_json', 'Threads::Threads', 'PkgConfig::MARIADB'},
        'monitor_runtime': {'monitor_application', 'monitor_options'},
    }
    found = set()
    for body in re.findall(r'target_link_libraries\s*\(([^)]*)\)', text, re.S):
        arguments = body.split()
        if not arguments or arguments[0] not in allowed:
            continue
        target = arguments[0]
        found.add(target)
        links = set(arguments[1:]) - {'PUBLIC', 'PRIVATE', 'INTERFACE'}
        assert links <= allowed[target], (target, links - allowed[target])
    assert found == set(allowed), ('Missing layer link declarations', set(allowed) - found)
    for body in re.findall(r'add_library\s*\(([^)]*)\)', text, re.S):
        arguments = body.split()
        if not arguments or arguments[0] not in allowed:
            continue
        layer = arguments[0][len('monitor_'):]
        for source in arguments[1:]:
            assert source.startswith('src/' + layer + '/') and source.endswith('.cpp'), (arguments[0], source)


def expect_rejected(operation, *arguments):
    try:
        operation(*arguments)
    except AssertionError:
        return
    raise AssertionError('Negative architecture fixture did not fail: ' + repr(arguments))


def main():
    root = pathlib.Path(sys.argv[1])
    count = 0
    for folder in ('src', 'tests'):
        for path in (root / folder).rglob('*'):
            if path.suffix not in ('.cpp', '.hpp', '.py'):
                continue
            relative = path.relative_to(root).as_posix()
            text = path.read_text(encoding='utf-8')
            lines = text.splitlines()
            prefix = '# ' if path.suffix == '.py' else '// '
            assert lines[0] == prefix + relative, relative + ': invalid path comment'
            if path.suffix == '.py':
                comments = [token for token in tokenize.generate_tokens(io.StringIO(text).readline) if token.type == tokenize.COMMENT]
                assert len(comments) == 1 and comments[0].start[0] == 1, relative
            else:
                assert not has_cpp_comment('\n'.join(lines[1:])), relative + ': extra comment'
                check_includes(relative, text)
            count += 1
    assert has_cpp_comment('int value; // forbidden')
    assert has_cpp_comment('int value; /* forbidden */')
    assert not has_cpp_comment('auto text = "https://example.org";')
    assert not has_cpp_comment('auto text = R"tag(// literal)tag";')
    for relative, header in (
            ('src/domain/Bad.cpp', 'infrastructure/Bad.hpp'),
            ('src/domain/Bad.cpp', '../application/MonitorSettings.hpp'),
            ('src/application/Bad.cpp', 'domain/../infrastructure/Bad.hpp'),
            ('src/application/Bad.cpp', '../infrastructure/Bad.hpp'),
            ('src/application/Bad.cpp', 'mysql.h'),
            ('src/domain/Bad.cpp', 'nlohmann/json.hpp'),
            ('src/domain/Bad.cpp', 'sys/stat.h'),
            ('src/application/Bad.cpp', 'pthread.h'),
            ('src/infrastructure/Bad.cpp', '../runtime/DaemonRunner.hpp'),
            ('src/runtime/Bad.cpp', 'infrastructure/database/MySqlConfiguration.hpp')):
        expect_rejected(check_includes, relative, '#include <' + header + '>')
    check_includes('src/application/Good.cpp', '#include "../domain/ResourceSample.hpp"\n#include <expected>')
    build = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    check_build_dependencies(build)
    expect_rejected(check_build_dependencies, build + '\ntarget_link_libraries(monitor_domain PRIVATE monitor_infrastructure)')
    expect_rejected(check_build_dependencies, build + '\nadd_library(monitor_application src/infrastructure/Bad.cpp)')
    print(f'Architecture and source comment rules passed: {count} files')


if __name__ == '__main__':
    main()

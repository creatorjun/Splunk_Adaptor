# tests/architecture_test.py
import io
import pathlib
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


def check_includes(relative, text):
    includes = re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.MULTILINE)
    for header in includes:
        if relative.startswith('src/domain/'):
            assert not header.startswith(('application/', 'infrastructure/', 'runtime/', 'nlohmann/', 'sys/')), (relative, header)
            assert header not in ('unistd.h', 'fcntl.h', 'signal.h', 'poll.h'), (relative, header)
        if relative.startswith('src/application/'):
            assert not header.startswith(('infrastructure/', 'runtime/', 'nlohmann/', 'sys/')), (relative, header)
            assert header not in ('unistd.h', 'fcntl.h', 'signal.h', 'poll.h'), (relative, header)
        if relative.startswith('src/infrastructure/'):
            assert not header.startswith('runtime/'), (relative, header)
        if relative.startswith('src/runtime/'):
            assert not header.startswith('nlohmann/'), (relative, header)


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
    try:
        check_includes('src/domain/Bad.cpp', '#include "infrastructure/Bad.hpp"')
    except AssertionError:
        pass
    else:
        raise AssertionError('Negative dependency fixture did not fail')
    print(f'Architecture and source comment rules passed: {count} files')


if __name__ == '__main__':
    main()

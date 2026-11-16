"""Build and run mode/patch tests in a temporary, isolated executable directory.

Tests include the actual implementation and exercise private simulated game
memory. They never attach to the game or edit its installed files.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    setup = (ROOT / 'build.bat').read_text(encoding='utf-8')
    setup = setup[:setup.index('cl /nologo')]
    with tempfile.TemporaryDirectory(prefix='mhw-startup-mode-tests-') as folder:
        batch = Path(folder) / 'run.bat'
        batch.write_text(
            setup
            + f'cl /nologo /EHsc /W4 /std:c++17 /utf-8 "{ROOT / "tests/mode_tests.cpp"}" '
              f'"{ROOT / "src/hook.cpp"}" /Fe:mode_tests.exe\n'
            + 'if errorlevel 1 exit /b 1\nmode_tests.exe\nexit /b %errorlevel%\n',
            encoding='utf-8',
        )
        subprocess.run(['cmd', '/c', str(batch)], check=True)


if __name__ == '__main__':
    main()

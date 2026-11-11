#include "common.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

int cli_main(int argc, char **argv);

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* Python sees the command line as Unicode (the CRT's wide argv); the
     * narrow argv is the ANSI code page and loses everything outside it. Take
     * the same wide words, split by the same CRT rules, as UTF-8. */
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        std::vector<std::string> words;
        for (int i = 0; i < wargc; i++) {
            const wchar_t *w = wargv[i];
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
            std::string s(n > 0 ? (size_t)n - 1 : 0, '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, 0, 0);
            words.push_back(s);
        }
        LocalFree(wargv);
        std::vector<char *> ptrs;
        for (size_t i = 0; i < words.size(); i++)
            ptrs.push_back(const_cast<char *>(words[i].c_str()));
        ptrs.push_back(0);
        return cli_main((int)words.size(), &ptrs[0]);
    }
#endif
    return cli_main(argc, argv);
}

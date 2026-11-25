#! /bin/sh
# mkpages.sh -- make docs/index.md (the GitHub Pages site) from README.md: the same page, with the image
# paths relative to docs/ and the front matter of the theme.  Run it after changing README.md.
set -e
top=$(cd "$(dirname "$0")/.." && pwd)
{
    printf -- '---\ntitle: wincan\ndescription: A candle chart for the Windows desktop, and the server that feeds it\n---\n\n'
    sed -e 's|docs/img/|img/|g' \
        -e 's|(COPYING)|(https://github.com/moebiusV/wincan/blob/main/COPYING)|g' \
        -e 's|(INSTALL)|(https://github.com/moebiusV/wincan/blob/main/INSTALL)|g' \
        -e 's|(HACKING)|(https://github.com/moebiusV/wincan/blob/main/HACKING)|g' \
        -e 's|(man/\([^)]*\))|(https://github.com/moebiusV/wincan/blob/main/man/\1)|g' \
        -e 's|(windows/README.txt)|(https://github.com/moebiusV/wincan/blob/main/windows/README.txt)|g' \
        -e 's|(grammars/candled.hbnf)|(https://github.com/moebiusV/wincan/blob/main/grammars/candled.hbnf)|g' \
        -e 's|(contrib/\([^)]*\))|(https://github.com/moebiusV/wincan/blob/main/contrib/\1)|g' \
        "$top/README.md"
} > "$top/docs/index.md"
echo "wrote docs/index.md"

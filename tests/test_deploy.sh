#!/bin/bash
# tests/test_deploy.sh — sony-xm3-deploy must never write or delete through a
# symbolic link, and must delete only what it is told to.
#
# Usage: tests/test_deploy.sh <path-to-sony-xm3-deploy>
set -uo pipefail

DEPLOY="${1:?usage: test_deploy.sh <sony-xm3-deploy>}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

HOME_DIR="$WORK/home"
VICTIM="$WORK/victim"
SRC="$WORK/src"
mkdir -p "$HOME_DIR" "$VICTIM" "$SRC"
chmod 0755 "$HOME_DIR"

pass=0
fail=0
ok() { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  FAIL %s\n' "$1"; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

deploy() { "$DEPLOY" --home "$HOME_DIR" "$@"; }

reset_victim() {
  rm -rf "$VICTIM"
  mkdir -p "$VICTIM"
  echo precious > "$VICTIM/keep.txt"
  echo precious > "$VICTIM/manifest.json"
}

printf 'one\n' > "$SRC/manifest.json"
printf 'two\n' > "$SRC/Panel.qml"
printf '#!/bin/sh\n' > "$SRC/tool"

echo "put"
deploy put .config/omarchy/plugins/demo 0644 "$SRC/manifest.json" "$SRC/Panel.qml" >/dev/null
check "creates missing directories and files" \
  '[[ -f $HOME_DIR/.config/omarchy/plugins/demo/Panel.qml ]]'
check "copies contents" \
  '[[ $(cat "$HOME_DIR/.config/omarchy/plugins/demo/manifest.json") == one ]]'
check "applies the requested mode" \
  '[[ $(stat -c %a "$HOME_DIR/.config/omarchy/plugins/demo/manifest.json") == 644 ]]'
deploy put .local/bin 0755 "$SRC/tool" >/dev/null
check "applies an executable mode" '[[ $(stat -c %a "$HOME_DIR/.local/bin/tool") == 755 ]]'
check "leaves no temporary files" \
  '[[ -z $(find "$HOME_DIR" -name "*.deploy.*") ]]'

printf 'newer\n' > "$SRC/manifest.json"
deploy put .config/omarchy/plugins/demo 0644 "$SRC/manifest.json" >/dev/null
check "replaces an existing file" \
  '[[ $(cat "$HOME_DIR/.config/omarchy/plugins/demo/manifest.json") == newer ]]'

reset_victim
rm -rf "$HOME_DIR/.config/omarchy/plugins/demo"
ln -s "$VICTIM" "$HOME_DIR/.config/omarchy/plugins/demo"
deploy put .config/omarchy/plugins/demo 0644 "$SRC/manifest.json" >/dev/null 2>&1
check "refuses a symlinked destination directory" '[[ $? -ne 0 ]]'
check "  and leaves the symlink target untouched" \
  '[[ $(cat "$VICTIM/manifest.json") == precious && -f $VICTIM/keep.txt ]]'
rm "$HOME_DIR/.config/omarchy/plugins/demo"

reset_victim
mv "$HOME_DIR/.config" "$WORK/real-config"
ln -s "$VICTIM" "$HOME_DIR/.config"
deploy put .config/omarchy/plugins/demo 0644 "$SRC/manifest.json" >/dev/null 2>&1
check "refuses a symlink higher up the path" '[[ $? -ne 0 ]]'
check "  and creates nothing in its target" '[[ ! -e $VICTIM/omarchy ]]'
rm "$HOME_DIR/.config"
mv "$WORK/real-config" "$HOME_DIR/.config"

reset_victim
mkdir -p "$HOME_DIR/.config/omarchy/plugins/demo"
ln -s "$VICTIM/manifest.json" "$HOME_DIR/.config/omarchy/plugins/demo/manifest.json"
deploy put .config/omarchy/plugins/demo 0644 "$SRC/manifest.json" >/dev/null
check "replaces a symlinked file with a real file" \
  '[[ ! -L $HOME_DIR/.config/omarchy/plugins/demo/manifest.json ]]'
check "  without writing through it" '[[ $(cat "$VICTIM/manifest.json") == precious ]]'

mkdir -p "$HOME_DIR/.config/omarchy/plugins/demo/Panel.qml.d"
mkdir -p "$SRC/dirsrc" && printf 'x\n' > "$SRC/dirsrc/Panel.qml.d"
deploy put .config/omarchy/plugins/demo 0644 "$SRC/dirsrc/Panel.qml.d" >/dev/null 2>&1
check "refuses to replace a directory with a file" '[[ $? -ne 0 ]]'

check "rejects '..' in the destination" \
  '! deploy put .config/../../escape 0644 "$SRC/tool" >/dev/null 2>&1'
check "rejects an absolute destination" \
  '! deploy put /tmp 0644 "$SRC/tool" >/dev/null 2>&1'

mkdir -p "$HOME_DIR/open"
chmod 0777 "$HOME_DIR/open"
check "refuses a world-writable directory" \
  '! deploy put open 0644 "$SRC/tool" >/dev/null 2>&1'
chmod 0755 "$HOME_DIR/open"

echo "put, installed copy"
INPLACE=".config/omarchy/plugins/inplace"
mkdir -p "$HOME_DIR/$INPLACE"
printf 'in place\n' > "$HOME_DIR/$INPLACE/manifest.json"
out=$(deploy put --unless-git-checkout "$INPLACE" 0644 "$HOME_DIR/$INPLACE/manifest.json")
check "running from the destination copies nothing" '[[ $? -eq 0 && $out == *"nothing to copy"* ]]'
check "  and keeps the file" '[[ $(cat "$HOME_DIR/$INPLACE/manifest.json") == "in place" ]]'

mkdir -p "$HOME_DIR/$INPLACE/.git"
deploy put --unless-git-checkout "$INPLACE" 0644 "$SRC/manifest.json" >/dev/null
check "leaves a different git checkout alone (exit 3)" '[[ $? -eq 3 ]]'
check "  without touching its files" '[[ $(cat "$HOME_DIR/$INPLACE/manifest.json") == "in place" ]]'

echo "remove"
STATE=".local/state/sony-xm3"
mkdir -p "$HOME_DIR/$STATE"
touch "$HOME_DIR/$STATE/status.json" "$HOME_DIR/$STATE/status.json.tmp.123" "$HOME_DIR/$STATE/other"
deploy remove "$STATE" status.json 'status.json.tmp.*' >/dev/null
check "removes named files and prefix matches" \
  '[[ ! -e $HOME_DIR/$STATE/status.json && ! -e $HOME_DIR/$STATE/status.json.tmp.123 ]]'
check "  and nothing else" '[[ -e $HOME_DIR/$STATE/other ]]'
out=$(deploy remove --rmdir "$STATE" status.json)
check "--rmdir keeps a directory that still holds files" '[[ -d $HOME_DIR/$STATE && $out == *"still holds"* ]]'
deploy remove --rmdir "$STATE" other >/dev/null
check "--rmdir removes the emptied directory" '[[ ! -e $HOME_DIR/$STATE ]]'

check "succeeds when the directory is already gone" \
  'deploy remove --rmdir "$STATE" status.json >/dev/null'

reset_victim
mkdir -p "$HOME_DIR/.config/omarchy/plugins"
ln -s "$VICTIM" "$HOME_DIR/.config/omarchy/plugins/linked"
deploy remove --rmdir .config/omarchy/plugins/linked manifest.json keep.txt >/dev/null 2>&1
check "refuses to remove through a symlinked directory" '[[ $? -ne 0 ]]'
check "  and deletes nothing in its target" \
  '[[ -f $VICTIM/manifest.json && -f $VICTIM/keep.txt ]]'
rm "$HOME_DIR/.config/omarchy/plugins/linked"

reset_victim
mkdir -p "$HOME_DIR/.local/bin"
ln -s "$VICTIM/keep.txt" "$HOME_DIR/.local/bin/sony-xm3-ctl"
deploy remove .local/bin sony-xm3-ctl >/dev/null
check "removes a symlinked file entry, not its target" \
  '[[ ! -L $HOME_DIR/.local/bin/sony-xm3-ctl && -f $VICTIM/keep.txt ]]'

mkdir -p "$HOME_DIR/.local/bin/sony-xm3-daemon"
touch "$HOME_DIR/.local/bin/sony-xm3-daemon/inner"
deploy remove .local/bin sony-xm3-daemon >/dev/null
check "never removes a directory entry" '[[ -f $HOME_DIR/.local/bin/sony-xm3-daemon/inner ]]'

check "rejects a name containing a slash" \
  '! deploy remove .local/bin ../x >/dev/null 2>&1'

echo
echo "test_deploy: $pass passed, $fail failed"
[[ $fail -eq 0 ]]

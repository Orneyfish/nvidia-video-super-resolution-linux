#!/bin/bash
# install.json helper for install/install.sh (bash + jq). Replaces the former manifest.py; same subcommands,
# stdout and exit codes. Schema and TSV layout: docs/install-layout.md.
#
#   to-json  OUT           read TSV on stdin, write OUT atomically; unchanged content leaves the file alone
#   to-tsv   FILE          print install.json as TSV
#   validate FILE          exit 0 when FILE is a well-formed install.json (schema 1), else print each problem, exit 1
#   remove-line FILE LINE  remove every line equal to LINE, atomically, keeping mode; prints the count
#
# TSV records: meta KEY VALUE | path KEY VALUE | comp NAME STATUS DETAIL |
#              entry TYPE PATH COMPONENT CREATED(0|1) BACKUP SHA256 EXTRA   ("-" = empty)
set -u

die() { printf 'manifest.sh: %s\n' "$1" >&2; exit "${2:-1}"; }
command -v jq >/dev/null 2>&1 || die "jq is required (sudo pacman -S jq)"

JQ_LIB='
def truthy: . != null and . != false and . != 0 and . != "" and . != [] and . != {};
def dash: if . == "-" or . == "" then null else . end;
def undash: if . == null or . == "" then "-" else tostring end;
def or_dash: if truthy then . else "-" end;
def q: [39] | implode;
def repr: if type == "string" then q + . + q elif . == null then "None"
  elif type == "boolean" then (if . then "True" else "False" end) else tostring end;
def STATUSES: ["installed", "skipped", "failed", "partial", "planned"];
def ENTRY_TYPES: ["dir", "file", "symlink", "line", "tree"];
def META_KEYS: ["version", "installed_at", "git_commit", "git_dirty", "repo", "prefix", "home", "config_home",
  "data_home", "nvvsr_libdir", "wheel_version"];

def entry_problems($i):
  . as $e
  | [ if (ENTRY_TYPES | any(. == $e.type)) then empty else "entry \($i) has type \($e.type | repr)" end,
      if ($e.path | type) == "string" and ($e.path | startswith("/")) then empty
        else "entry \($i) has a non-absolute path" end,
      if ($e.created | type) == "boolean" then empty else "entry \($i): created must be a boolean" end,
      if $e.backup == null or (($e.backup | type) == "string" and ($e.backup | startswith("/"))) then empty
        else "entry \($i): backup must be an absolute path or null" end,
      if $e.sha256 == null or (($e.sha256 | type) == "string" and ($e.sha256 | test("^[0-9a-f]{64}\\z"))) then empty
        else "entry \($i): sha256 must be 64 hex digits or null" end,
      if $e.type == "line" and ($e.line | truthy | not) then "entry \($i): a line entry needs \(q)line\(q)"
        else empty end,
      if $e.type == "symlink" and ($e.target | truthy | not) then "entry \($i): a symlink entry needs \(q)target\(q)"
        else empty end ];

def problems:
  if type != "object" then ["not a JSON object"] else
  [ if .schema != 1 then "schema is \(.schema | repr), expected 1" else empty end,
    (["version", "installed_at", "updated_at", "prefix", "home", "repo"][] as $k
      | if (.[$k] | type) == "string" and (.[$k] | length) > 0 then empty
        else "missing or empty \($k | repr)" end),
    (["prefix", "home", "repo"][] as $k
      | if (.[$k] | type) == "string" and (.[$k] | startswith("/") | not) then "\($k | repr) is not absolute"
        else empty end),
    (if .git_commit != null and (.git_commit | type) != "string" then "git_commit must be a string or null" else empty end),
    (if (.git_dirty | type) != "boolean" then "git_dirty must be a boolean" else empty end),
    (if .nvvsr_libdir != null and (.nvvsr_libdir | type) != "string" then "nvvsr_libdir must be a string or null"
      else empty end),
    (if (.paths | type) != "object" then "paths must be an object" else empty end),
    (if (.components | type) != "object" or (.components | length) == 0 then "components must be a non-empty object"
      else (.components | to_entries[]
        | select((.value | type) != "object" or ((.value.status) as $s | (STATUSES | any(. == $s)) | not))
        | "component \(.key | repr) has a bad status") end),
    (if (.entries | type) != "array" then "entries must be an array"
      else (.entries | to_entries[] | .key as $i | .value as $e
        | if ($e | type) != "object" then "entry \($i) is not an object" else ($e | entry_problems($i) | .[]) end) end)
  ] end;

def parse_tsv:
  reduce (split("\n") | to_entries[]) as $L (
    {doc: {schema: 1, paths: {}, components: {}, entries: []}, wheel: {}, mpv: {}};
    ($L.value | split("\t")) as $f
    | ($L.key + 1) as $n
    | if $L.value == "" then .
      elif $f[0] == "meta" and ($f | length) == 3 then
        ($f[1]) as $k | ($f[2]) as $v
        | if $k == "wheel_version" then .doc.wheel_version = ($v | dash)
          elif ($k | startswith("wheel_")) then .wheel[($k | .[6:])] = $v
          elif ($k | startswith("mpv_")) then .mpv[($k | .[4:])] = $v
          elif $k == "git_dirty" then .doc[$k] = ($v == "1")
          elif $k == "git_commit" or $k == "nvvsr_libdir" then .doc[$k] = ($v | dash)
          else .doc[$k] = $v end
      elif $f[0] == "path" and ($f | length) == 3 then .doc.paths[$f[1]] = ($f[2] | dash)
      elif $f[0] == "comp" and ($f | length) == 4 then
        .doc.components[$f[1]] = {status: $f[2], detail: (if $f[3] == "-" then "" else $f[3] end)}
      elif $f[0] == "entry" and ($f | length) == 8 then
        .doc.entries += [{type: $f[1], path: $f[2], component: $f[3], created: ($f[4] == "1"),
          backup: ($f[5] | dash), sha256: ($f[6] | dash)}
          + (if $f[1] == "symlink" then {target: ($f[7] | dash)}
             elif $f[1] == "line" then {line: ($f[7] | dash)} else {} end)]
      else error("bad TSV record on line \($n)") end)
  | . as $s
  | $s.doc
  | (if ($s.wheel | length) > 0 then .wheel = {name: ($s.wheel.name // "nvidia-vfx"),
      sha256: (($s.wheel.sha256 // "-") | dash), installed: ($s.wheel.installed == "1")} else . end)
  | (if ($s.mpv | length) > 0 then .private_mpv = {path: $s.mpv.path,
      vapoursynth: (if $s.mpv.vapoursynth == "1" then true elif $s.mpv.vapoursynth == "0" then false else null end),
      built_by_installer: ($s.mpv.built == "1")} else . end);

def to_tsv:
  (META_KEYS[] as $k | select(has($k))
    | "meta\t\($k)\t\(.[$k] | if . == true then "1" elif . == false then "0" else undash end)"),
  ((.wheel // {}) as $w | if ($w | length) > 0 then (
      "meta\twheel_name\t\($w.name | undash)",
      "meta\twheel_sha256\t\($w.sha256 | undash)",
      "meta\twheel_installed\t\(if $w.installed then "1" else "0" end)") else empty end),
  ((.private_mpv // {}) as $m | if ($m | length) > 0 then (
      "meta\tmpv_path\t\($m.path | undash)",
      "meta\tmpv_vapoursynth\t\(if $m.vapoursynth == true then "1" elif $m.vapoursynth == false then "0" else "-" end)",
      "meta\tmpv_built\t\(if $m.built_by_installer then "1" else "0" end)") else empty end),
  ((.paths // {}) | to_entries[] | "path\t\(.key)\t\(.value | undash)"),
  ((.components // {}) | to_entries[] | "comp\t\(.key)\t\(.value.status // "")\t\(.value.detail | undash)"),
  ((.entries // [])[] | "entry\t\(.type)\t\(.path)\t\(.component | or_dash)\t\(if .created then "1" else "0" end)\t\(.backup | undash)\t\(.sha256 | undash)\t\(if .type == "symlink" then .target elif .type == "line" then .line else null end | undash)");
'

JQ_TO_JSON='
parse_tsv
| (if has("installed_at") then . else .installed_at = $now end)
| .updated_at = $now
| if ($old | type) == "object" and ($old.installed_at | truthy) then .installed_at = $old.installed_at else . end
| if ($old | type) == "object" and ((. + {updated_at: $old.updated_at}) == $old) then "unchanged"
  else . as $n
    | reduce (["schema", "version", "installed_at", "updated_at", "git_commit", "git_dirty", "repo", "prefix", "home",
               "config_home", "data_home", "nvvsr_libdir", "wheel_version", "paths", "private_mpv", "wheel", "components",
               "entries"][]) as $k
        ({}; if ($n | has($k)) then .[$k] = $n[$k] else . end)
  end'

JQ_PROBLEMS='problems | join("; ")'
JQ_VALIDATE='problems[]'
JQ_TO_TSV='to_tsv'

# Atomic replace: write the new content to a temp file in the same directory, then rename it over FILE.
replace_file() { # TMP FILE: moves TMP to FILE; keeps FILE's mode, or 644 for a new file
  if [ -e "$2" ]; then chmod --reference="$2" "$1"; else chmod 644 "$1"; fi
  sync "$1"
  mv -f -- "$1" "$2" || { rm -f -- "$1"; die "cannot replace $2"; }
}

load_json() { # FILE -> DOC: the single JSON value in FILE, compact; dies when unreadable or not JSON
  [ -r "$1" ] || die "cannot read $1: $([ -e "$1" ] && echo "Permission denied" || echo "No such file or directory")"
  DOC=$(jq -cs 'if length == 1 then .[0] else error("extra data") end' "$1" 2>/dev/null) || die "$1 is not valid JSON"
}

cmd_to_json() {
  local out=$1 now old o ordered bad tmp
  now=$(date -u +%Y-%m-%dT%H:%M:%S+00:00)
  old=null
  if [ -f "$out" ]; then o=$(jq -c 'select(type == "object")' "$out" 2>/dev/null) && [ -n "$o" ] && old=$o; fi
  ordered=$(jq -Rsr --indent 2 --arg now "$now" --argjson old "$old" "$JQ_LIB $JQ_TO_JSON" 2>&1) || die "$ordered"
  [ "$ordered" = unchanged ] && return 0
  bad=$(printf '%s\n' "$ordered" | jq -r "$JQ_LIB $JQ_PROBLEMS") || die "cannot check the new install.json"
  if [ -n "$bad" ]; then die "refusing to write an invalid install.json: $bad"; fi
  tmp=$(mktemp "$(dirname -- "$out")/.install.json.XXXXXX") || die "cannot write $out"
  printf '%s\n' "$ordered" >"$tmp" || { rm -f -- "$tmp"; die "cannot write $out"; }
  replace_file "$tmp" "$out"
}

cmd_to_tsv() {
  load_json "$1"
  printf '%s\n' "$DOC" | jq -r "$JQ_LIB $JQ_TO_TSV" || die "$1 is not an install.json object"
}

cmd_validate() {
  local bad
  load_json "$1"
  bad=$(printf '%s\n' "$DOC" | jq -r "$JQ_LIB $JQ_VALIDATE") || die "cannot check $1"
  if [ -n "$bad" ]; then printf '%s\n' "$bad"; return 1; fi
}

cmd_remove_line() {
  local f=$1 line=$2 n tmp
  [ -r "$f" ] || die "cannot read $f: $([ -e "$f" ] && echo "Permission denied" || echo "No such file or directory")"
  n=$(jq -Rs --arg t "$line" '[split("\n")[] | select(. == $t)] | length' "$f") || die "cannot read $f"
  if [ "$n" -gt 0 ]; then
    tmp=$(mktemp "$(dirname -- "$f")/.install.json.XXXXXX") || die "cannot write $f"
    jq -Rsj --arg t "$line" 'split("\n") | map(select(. != $t)) | join("\n")' "$f" >"$tmp" \
      || { rm -f -- "$tmp"; die "cannot write $f"; }
    replace_file "$tmp" "$f"
  fi
  printf '%s\n' "$n"
}

[ $# -ge 1 ] || die "usage: manifest.sh to-json OUT | to-tsv FILE | validate FILE | remove-line FILE LINE" 2
cmd=$1; shift
case $cmd:$# in
  to-json:1) cmd_to_json "$1" ;;
  to-tsv:1) cmd_to_tsv "$1" ;;
  validate:1) cmd_validate "$1" ;;
  remove-line:2) cmd_remove_line "$1" "$2" ;;
  *) die "bad arguments for $cmd" 2 ;;
esac

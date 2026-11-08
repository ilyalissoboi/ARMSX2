#!/usr/bin/env bash
# Prints the body of an ilyalissoboi/ARMSX2 fork release (used by fork-release.yml).
#
# The top part is hand-written: .github/fork-release-notes/<VN>.md in the released commit,
# holding the intro, "What's new in this fork" and "From upstream". Everything after it is
# generated here: the Discord note, which APK to install, the signing note, the SHA-256
# table and the source line.
#
# With no notes file the top part is a marked placeholder plus GitHub's list of pull
# requests merged since the previous fork release, and fork-release.yml leaves the release
# as a draft so it can't go out like that.
#
# Env:
#   VN        versionName / tag, e.g. 2.9.2-fork.1
#   SHA       the released commit
#   REPO      owner/name
#   APK_DIR   directory holding the four ARMSX2-<VN>-*.apk files
#   PREV_TAG  previous published fork release (optional)
#   GH_TOKEN  for the gh calls
set -euo pipefail

VN="${VN:?}"
SHA="${SHA:?}"
REPO="${REPO:?}"
APK_DIR="${APK_DIR:?}"
PREV_TAG="${PREV_TAG:-}"
NOTES_FILE="${NOTES_FILE:-.github/fork-release-notes/$VN.md}"

# Tier suffix | who it's for. The suffixes are the updater's contract
# (see platforms/android/tools/build-release-targets.sh); keep the legacy one first.
TIERS=(
	"legacy-armv8.0-sdk26|Android 8 and up, any ARMv8 CPU"
	"a11-armv8.2-sdk30|Android 11 and up, ARMv8.2 CPU with FP16 and DotProd"
	"a13-armv8.2-sdk33|Android 13 and up, same CPU requirement"
	"a15-armv8.2-sdk35|Android 15 and up, same CPU requirement"
)

# ---- hand-written part ------------------------------------------------------------------
if [[ -f "$NOTES_FILE" ]]; then
	cat "$NOTES_FILE"
	echo
else
	echo "> **Draft:** this commit has no \`$NOTES_FILE\`, so below is GitHub's list of pull requests merged since ${PREV_TAG:-the start}. Replace it with the fork's changes and the upstream status before publishing."
	echo
	args=(-f tag_name="$VN" -f target_commitish="$SHA")
	[[ -z "$PREV_TAG" ]] || args+=(-f previous_tag_name="$PREV_TAG")
	gh api "repos/$REPO/releases/generate-notes" "${args[@]}" --jq .body
	echo
fi
echo

# ---- generated part ---------------------------------------------------------------------
cat <<'EOF'
**Not in this build:** Discord. Upstream's Discord features need Discord's SDK, which upstream ships privately and this fork's build doesn't have.

**Which APK to install:**

Same tiers as upstream. Install the highest one your device can run.

EOF
for t in "${TIERS[@]}"; do
	echo "- ARMSX2-$VN-${t%%|*}.apk: ${t#*|}"
done
echo
echo "Each APK carries both the 4K-page and 16K-page cores and is built with PGO. Memory card sync works in every tier."
echo

# Every published fork release, newest first, without this one.
mapfile -t EARLIER < <(gh release list -R "$REPO" --exclude-drafts -L 200 --json tagName \
	--jq '.[].tagName | select(test("-fork[.][0-9]+$"))' | grep -vxF "$VN" || true)
case "${#EARLIER[@]}" in
	0) same="these APKs are signed with this fork's own key" ;;
	1) same="these APKs are signed with the same key as ${EARLIER[0]} and install over it" ;;
	*) same="these APKs are signed with the same key as every earlier release of this fork, from ${EARLIER[-1]} to ${EARLIER[0]}, and install over them" ;;
esac
echo "**Signing note:** $same. They use the package name com.armsx2 but are signed with this fork's own key, so Android will refuse to install them over an upstream ARMSX2 install. Uninstall upstream's build first, and back up your data folder before you do. The in-app news feed still shows upstream's release notes."
echo
echo "| File | SHA-256 |"
echo "|---|---|"
for t in "${TIERS[@]}"; do
	f="ARMSX2-$VN-${t%%|*}.apk"
	[[ -f "$APK_DIR/$f" ]] || { echo "release-notes.sh: missing $APK_DIR/$f" >&2; exit 1; }
	echo "| \`$f\` | \`$(sha256sum "$APK_DIR/$f" | cut -d' ' -f1)\` |"
done
echo
echo "Source: https://github.com/$REPO (tag $VN, commit ${SHA:0:10})."

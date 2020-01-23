#!/bin/bash
set -euo pipefail

# --- begin runfiles.bash initialization ---
# Copy-pasted from Bazel's Bash runfiles library https://github.com/bazelbuild/bazel/blob/defd737761be2b154908646121de47c30434ed51/tools/bash/runfiles/runfiles.bash
if [[ ! -d "${RUNFILES_DIR:-/dev/null}" && ! -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
  if [[ -f "$0.runfiles_manifest" ]]; then
    export RUNFILES_MANIFEST_FILE="$0.runfiles_manifest"
  elif [[ -f "$0.runfiles/MANIFEST" ]]; then
    export RUNFILES_MANIFEST_FILE="$0.runfiles/MANIFEST"
  elif [[ -f "$0.runfiles/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
    export RUNFILES_DIR="$0.runfiles"
  fi
fi
if [[ -f "${RUNFILES_DIR:-/dev/null}/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
  # shellcheck disable=SC1090
  source "${RUNFILES_DIR}/bazel_tools/tools/bash/runfiles/runfiles.bash"
elif [[ -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
  # shellcheck disable=SC1090
  source "$(grep -m1 "^bazel_tools/tools/bash/runfiles/runfiles.bash " \
            "$RUNFILES_MANIFEST_FILE" | cut -d ' ' -f 2-)"
else
  echo >&2 "ERROR: cannot find @bazel_tools//tools/bash/runfiles:runfiles.bash"
  exit 1
fi
# --- end runfiles.bash initialization ---

# Find logging with rlocation, as this script is run from a genrule
# shellcheck disable=SC1090
source "$(rlocation com_stripe_sorbet_llvm/test/logging.sh)"

# Argument Parsing #############################################################

rbout=${1}
rberr=${2}
rbexit=${3}
rb=${4}

# Environment Setup ############################################################

# NOTE: using a temp file here, as that will cause ruby to not print the name of
# the main file in a stack trace.
rbrunfile=$(mktemp)

# Find ruby
ruby="$(rlocation sorbet_ruby/ruby)"

# Determine the location of run/tools
run_tools=$(dirname "$(rlocation com_stripe_sorbet_llvm/run/tools/preamble.rb)")

# Main #########################################################################

info "--- Build Config ---"
info "* Test:   ${rb}"
info "* Rbout:  ${rbout}"
info "* Rberr:  ${rberr}"
info "* Rbexit: ${rbexit}"
info "* Runner: ${rbrunfile}"

info "--- Debugging ---"
info "    test/run_ruby.sh -d ${rb}"

info "--- Building output ---"
echo "require './$rb';" > "$rbrunfile"

# Use a temp directory for LLVMIR so we don't accidentally pick up changes from
# the environment
llvmir=$(mktemp -d)
cleanup() {
    rm -r "$llvmir"
}
trap cleanup EXIT

set +e
# NOTE: we run with patch_require incluced so that the stack trace looks similar
# to what we'll see in the compiled version
llvmir="$llvmir" "$ruby" \
  --disable=gems \
  --disable=did_you_mean \
  -I "$run_tools" -rpreamble.rb -rpatch_require.rb \
  "$rbrunfile" > "$rbout" 2> "$rberr"
echo "$?" > "$rbexit"
set -e

info "Cleaning up"
rm "$rbrunfile"

success "* Captured output for ${rb}"

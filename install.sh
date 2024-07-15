#!/bin/bash

{ # this ensures the entire script is downloaded #

#
# This script should be run via curl:
#   sh -c "$(curl -fsSL https://raw.githubusercontent.com/pxlapp/postgres/pxl/install.sh)"
# or via wget:
#   sh -c "$(wget -qO- https://raw.githubusercontent.com/pxlapp/postgres/pxl/install.sh)"
# or via fetch:
#   sh -c "$(fetch -o - https://raw.githubusercontent.com/pxlapp/postgres/pxl/install.sh)"
#
# As an alternative, you can first download the install script and run it afterwards:
#   wget https://raw.githubusercontent.com/pxlapp/postgres/pxl/install.sh
#   sh install.sh

set -e

# Make sure important variables exist if not already defined
#
# $USER is defined by login(1) which is not always executed (e.g. containers)
# POSIX: https://pubs.opengroup.org/onlinepubs/009695299/utilities/id.html
USER=${USER:-$(id -u -n)}
# $HOME is defined at the time of login, but it could be unset. If it is unset,
# a tilde by itself (~) will not be expanded to the current user's home directory.
# POSIX: https://pubs.opengroup.org/onlinepubs/009696899/basedefs/xbd_chap08.html#tag_08_03
HOME="${HOME:-$(getent passwd $USER 2>/dev/null | cut -d: -f6)}"
# macOS does not have getent, but this works even if $HOME is unset
HOME="${HOME:-$(eval echo ~$USER)}"


if [ -z "${PXL_INSTALL_DIR:-}" ]; then
  case "$(uname -s)" in
  Darwin)
    PXL_INSTALL_DIR_RAW="\${HOME}/bin"
    ;;
  Linux)
    PXL_INSTALL_DIR_RAW="\${XDG_CACHE_HOME:-\${HOME}}/bin"
    ;;
  esac
  eval PXL_INSTALL_DIR="${PXL_INSTALL_DIR_RAW}"
fi

if [ ! "$(type -P curl)"  ]; then
    echo "No curl detected in the PATH. Please, install curl before installing PXL"
    exit 1
fi

#  This must be in the form <url>/<channel>
# eg. https://github.com/pxlapp/postgres/releases/download/stable
PXL_DIST_URL="${PXL_DIST_URL:-https://github.com/pxlapp/postgres/releases/download/stable}"
PXL_CHANNEL="$(basename "${PXL_DIST_URL}")"
PXL_INSTALL_NAME="pxl-${PXL_CHANNEL}"
PXL_EXE_RAW="${PXL_INSTALL_DIR}/${PXL_INSTALL_NAME}"
PXL_EXE="${PXL_INSTALL_DIR}/pxl"
PXL_EXE_DIR="$(dirname "${PXL_EXE}")"

ID_USER=$(id -u)
ID_GROUP=$(id -g)

install_binary() {
  if [ ! -e "${PXL_EXE_DIR}" ]; then
    echo "=> Creating ${PXL_EXE_DIR}"
    mkdir -p "${PXL_EXE_DIR}"
    chown "$ID_USER:$ID_GROUP" "${PXL_EXE_DIR}"
  fi

  if [ ! -w "${PXL_EXE_DIR}" ]; then
    echo "${PXL_EXE_DIR} is not writeable, making it so"
    chown "$ID_USER:$ID_GROUP" "${PXL_EXE_DIR}"
    chmod u+w "${PXL_EXE_DIR}"
  fi

  OS="$(uname -s | tr A-Z a-z)"
  ARCH="$(uname -m | tr A-Z a-z)"
  if [ "$ARCH" = "x86_64" ]; then
    ARCH="amd64"
  elif [ "$ARCH" = "aarch64" ]; then
    ARCH="arm64"
  fi
  URL="${PXL_DIST_URL}/pxl-${OS}-${ARCH}"
  TMP_FILE="${PXL_EXE_RAW}.download.${RANDOM}"
  echo "=> Downloading ${URL} to ${PXL_EXE}"
  chmod -f u+w "${PXL_EXE_RAW}" 2> /dev/null || true
  curl -fsSL "${URL}" -o "${TMP_FILE}"
  chown "$ID_USER:$ID_GROUP" "${TMP_FILE}"
  chmod u+wx "${TMP_FILE}"
  mv "${TMP_FILE}" "${PXL_EXE_RAW}"

  ln -fs "${PXL_INSTALL_NAME}" "${PXL_EXE}"

  echo "=> PXL installed as ${PXL_EXE}"
}

pxl_detect_profile() {
  if [ "${PROFILE-}" = '/dev/null' ]; then
    # the user has specifically requested NOT to have pxl touch their profile
    return
  fi

  if [ -n "${PROFILE}" ] && [ -f "${PROFILE}" ]; then
    echo "${PROFILE}"
    return
  fi

  DETECTED_PROFILE=''

  if [ "${SHELL#*bash}" != "$SHELL" ]; then
    if [ -f "$HOME/.bashrc" ]; then
      DETECTED_PROFILE="$HOME/.bashrc"
    elif [ -f "$HOME/.bash_profile" ]; then
      DETECTED_PROFILE="$HOME/.bash_profile"
    fi
  elif [ "${SHELL#*zsh}" != "$SHELL" ]; then
    if [ -f "$HOME/.zshrc" ]; then
      DETECTED_PROFILE="$HOME/.zshrc"
    elif [ -f "$HOME/.zprofile" ]; then
      DETECTED_PROFILE="$HOME/.zprofile"
    fi
  fi

  if [ -n "$DETECTED_PROFILE" ]; then
    echo "$DETECTED_PROFILE"
  fi
}

install_profile() {
  #SOURCE_STR="\n# PXL; START.\nalias psql=\"${PXL_EXE}\"\n# PXL; END."
  SOURCE_STR="alias psql=\"${PXL_EXE}\""
  read -n 1 -p "Should psql be aliased to PXL in your shell? (Y/n): " answer
  if [ -z "$answer" ]; then
  	answer="Y"
  fi
  if [ "$answer" = "Y" -o "$answer" = "y" ]; then
    echo "=> Detecting profile"
    PXL_PROFILE="$(pxl_detect_profile)"
    if [ -z "${PXL_PROFILE-}" ]; then
      echo "=> Profile not found. Tried ~/.bashrc, ~/.bash_profile, ~/.zprofile, ~/.zshrc, and ~/.profile."
      echo "=> Create one of them and run this script again"
      echo "   OR"
      echo "=> Append the following lines to the correct file yourself:"
      echo
      echo "${SOURCE_STR}"
      echo
    else
      echo "=> Aliasing psql to PXL in $PXL_PROFILE"
      echo >> "$PXL_PROFILE"
      echo "# PXL; START." >> "$PXL_PROFILE"
      echo "${SOURCE_STR}" >> "$PXL_PROFILE"
      echo "# PXL; END." >> "$PXL_PROFILE"
      echo
      echo "Please restart your shell or run \`source ${PXL_PROFILE}\` for the changes to take effect."
    fi
  else
      echo "=> Append the following lines to your shell profile:"
      echo
      echo "${SOURCE_STR}"
  fi
}


install_binary
install_profile </dev/tty

} # this ensures the entire script is downloaded #

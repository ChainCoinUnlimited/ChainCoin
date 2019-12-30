// Copyright (c) 2011-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_GUICONSTANTS_H
#define BITCOIN_QT_GUICONSTANTS_H

#include <cstdint>

/* Milliseconds between model updates */
static const int MODEL_UPDATE_DELAY = 250;

/* AskPassphraseDialog -- Maximum passphrase length */
static const int MAX_PASSPHRASE_SIZE = 1024;

/* ChaincoinGUI -- Size of icons in status bar */
static const int STATUSBAR_ICONSIZE = 16;

static const bool DEFAULT_SPLASHSCREEN = true;

/* Invalid field background style */
#define STYLE_INVALID "background:#FF8080"

/* List view -- unconfirmed transaction */
#define COLOR_UNCONFIRMED QColor(128, 128, 128)
/* List view -- negative amount */
#define COLOR_NEGATIVE QColor(255, 0, 0)
/* List view -- bare address (without label) */
#define COLOR_BAREADDRESS QColor(140, 140, 140)
/* List view -- status decoration - open until date */
#define COLOR_TX_STATUS_OPENUNTILDATE QColor(64, 64, 255)
/* List view -- status decoration - danger, tx needs attention */
#define COLOR_TX_STATUS_DANGER QColor(200, 100, 100)
/* List view -- status decoration - default color */
#define COLOR_BLACK QColor(3, 3, 3)
/* List view -- general green */
#define COLOR_GREEN QColor(23, 168, 26)

/* Tooltips longer than this (in characters) are converted into rich text,
   so that they can be word-wrapped.
 */
static const int TOOLTIP_WRAP_THRESHOLD = 80;

/* Number of frames in spinner animation */
#define SPINNER_FRAMES 36

#define QAPP_ORG_NAME "Chaincoin"
#define QAPP_ORG_DOMAIN "chaincoin.org"
#define QAPP_APP_NAME_DEFAULT "Chaincoin-Qt"
#define QAPP_APP_NAME_TESTNET "Chaincoin-Qt-testnet"
#define QAPP_APP_NAME_REGTEST "Chaincoin-Qt-regtest"

/* One gigabyte (GB) in bytes */
static constexpr uint64_t GB_BYTES{1000000000};

#endif // BITCOIN_QT_GUICONSTANTS_H

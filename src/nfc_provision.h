/**
 * @file      nfc_provision.h
 * @brief     NFC tag credential provisioning.
 *
 * An NDEF Text record whose payload matches `lilygo+<slot>:<value>` is
 * interpreted as a credential drop and routed to hal::secret_store under
 * the slot's (namespace, key) mapping. Known slots:
 *
 *   lilygo+tg:<bearer>   -> tgbridge/token_enc   (Telegram bearer)
 *   lilygo+gh:<pat>      -> notesync/token_enc   (GitHub PAT)
 *
 * The user always gets a confirm dialog with a masked preview before the
 * secret is written. If notes_crypto is locked, the dialog explains the
 * problem rather than silently failing.
 */
#pragma once

#include <cstddef>

/* Returns true if `text` looked like a provisioning payload (regardless of
 * whether the user ultimately confirmed the write). Caller should skip the
 * generic NFC-text popup in that case. */
bool nfc_provision_maybe_handle(const char *text, size_t len);

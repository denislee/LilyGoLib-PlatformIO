/**
 * @file      result.cpp
 * @brief     hal_error_string() — see result.h.
 */
#include "result.h"

const char *hal_error_string(HalError e)
{
    switch (e) {
    case HalError::Ok:                  return "OK";
    case HalError::InvalidArgument:     return "Invalid argument.";
    case HalError::NotInitialized:      return "Not initialized.";
    case HalError::NotSupported:        return "Not supported on this device.";
    case HalError::Timeout:             return "Operation timed out.";
    case HalError::InternalError:       return "Internal error.";

    case HalError::PathNotFound:        return "File not found.";
    case HalError::PathTooLong:         return "Path is too long.";
    case HalError::CannotOpen:          return "Cannot open file.";
    case HalError::ReadFailed:          return "Read failed.";
    case HalError::WriteFailed:         return "Write failed.";
    case HalError::StorageFull:         return "Storage is full.";
    case HalError::StorageOffline:      return "Storage is offline.";
    case HalError::DecodeFailed:        return "Could not decode file (wrong passphrase?).";

    case HalError::WifiOffline:         return "WiFi is offline.";
    case HalError::WifiAuthFailed:      return "WiFi authentication failed.";
    case HalError::DnsFailed:           return "DNS lookup failed.";
    case HalError::NetworkUnreachable:  return "Network unreachable.";
    case HalError::HttpError:           return "Server returned an error.";
    case HalError::Unauthorized:        return "Unauthorized (check token).";

    case HalError::HubDisabled:         return "Local hub is disabled.";
    case HalError::HubUnreachable:      return "Local hub is unreachable.";
    case HalError::HubBadResponse:      return "Local hub returned an invalid response.";
    }
    return "Unknown error.";
}

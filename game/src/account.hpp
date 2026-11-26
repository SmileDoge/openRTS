#pragma once

// The game's account on dark-team.ru: the site's launcher API
// (/api/launcher/*) signs the player in, keeps a token for 72 hours and
// checks the machine's hardware id against the site's bans.

#include <string>

namespace openrts::game {

// What the machine tells the site about itself. `hwid` is a stable hash of
// the Windows installation, the system disk and the first network card; the
// rest is shown to the staff when they decide about a hardware ban.
struct HardwareInfo {
    std::string hwid;
    std::string motherboard;
    std::string disk_serial;
    std::string mac_address;
    std::string cpu_name;
    std::string gpu_name;
    int ram_total_mb = 0;
};

struct AccountUser {
    std::string username;
    std::string email;
    std::string role;
    // The site's human-readable "Должность" (lk.html), e.g. "Участник проекта".
    std::string position;
};

struct AccountResult {
    bool ok = false;
    int status = 0; // 0: the site was not reached
    std::string error;
    std::string token;
    AccountUser user;
};

// Reads the hardware once; the calls below are blocking (call them on a
// worker, not on the frame's thread).
[[nodiscard]] HardwareInfo hardware_info();

[[nodiscard]] AccountResult account_login(const std::string& email, const std::string& password, const std::string& hwid);
[[nodiscard]] AccountResult account_validate(const std::string& token);
void account_logout(const std::string& token);
// The site's answer to a hardware ban check: `banned` and the staff's reason.
[[nodiscard]] bool account_hwid_banned(const std::string& hwid, std::string& reason);
// The components behind the hash, for the staff's ban decisions.
void account_report_hwid(const std::string& token, const HardwareInfo& info);

// The token and the last known user, kept between runs in
// %LOCALAPPDATA%/openRTS/account.ini.
[[nodiscard]] std::string account_token_file();
[[nodiscard]] bool load_account(std::string& token, AccountUser& user);
void save_account(const std::string& token, const AccountUser& user);
void clear_account();

} // namespace openrts::game

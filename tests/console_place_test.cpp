// The dispatcher's console puts a consist on the server's track by its cars
// (viewer/src/console_panel.hpp, consist_place): a .con from anywhere on the
// dispatcher's computer too (the map's "Browse...": a path the server does
// not have). Until 2026-11-21 such a file gave "consist not found" (the
// VFS only), the testers' freight consists among them.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "console_panel.hpp"

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using namespace openrts;
    namespace fs = std::filesystem;
    const auto folder = fs::temp_directory_path() / fs::path(u8"openrts_console_place_тест") / fs::path(u8"Грузовые поезда");
    std::error_code error_code;
    fs::create_directories(folder, error_code);
    const auto file = folder / fs::path(u8"rzd_cargo23 50vag.con");
    {
        std::ofstream out(file, std::ios::binary);
        out << "SIMISA@@@@@@@@@@JINX0D0t______\r\n\r\n"
               "Train (\r\n\tTrainCfg ( \"rzd_cargo23\"\r\n\t\tName ( \"rzd_cargo23\" )\r\n\t\tSerial ( 1 )\r\n"
               "\t\tMaxVelocity ( 22.2 0.1 )\r\n\t\tNextWagonUID ( 3 )\r\n\t\tDurability ( 1.00 )\r\n"
               "\t\tEngine ( UiD ( 0 ) EngineData ( VL80s-436A \"mdd VL80s-436\" ) )\r\n"
               "\t\tWagon ( UiD ( 1 ) WagonData ( cargo4_1 \"rzd cargo\" ) )\r\n"
               "\t\tWagon ( UiD ( 2 ) Flip ( ) WagonData ( cargo4_2 \"rzd cargo\" ) )\r\n\t)\r\n)\r\n";
    }
    const core::Vfs vfs;
    std::string error;
    const auto text = file.u8string();
    const auto place = viewer::consist_place(vfs, std::string(text.begin(), text.end()), error);
    check(place.has_value(), "a .con by its absolute path (a Cyrillic folder): read here: " + error);
    if (place) {
        check(place->cars.size() == 3, "its three cars");
        // Folded to lower case, as the consist parser keeps them (the server
        // looks the files up without case).
        check(place->cars.size() == 3 && place->cars[0].engine && place->cars[0].folder == u"mdd vl80s-436" && place->cars[0].name == u"vl80s-436a",
              "the engine first, its folder and name");
        check(place->cars.size() == 3 && !place->cars[1].engine && place->cars[2].flipped, "the wagons, the last flipped");
        check(place->consist == u"rzd_cargo23 50vag", "named by its file");
    }
    std::string missing_error;
    check(!viewer::consist_place(vfs, "TRAINS/CONSISTS/none at all.con", missing_error) && !missing_error.empty(),
          "a consist neither on this computer nor in the VFS: an error");
    fs::remove_all(folder.parent_path(), error_code);
    if (failures == 0) std::cout << "console_place: ok\n";
    return failures == 0 ? 0 : 1;
}

#include "anvil/http/csv_writer.h"

namespace anvil::http {
namespace {

[[nodiscard]] constexpr bool needs_quoting(std::string_view cell) noexcept {
    for (const char c : cell) {
        if (c == ',' || c == '"' || c == '\r' || c == '\n') { return true; }
    }
    // A leading or trailing space survives unquoted in every importer that
    // matters, so it is deliberately not a reason to quote — quoting every cell
    // would inflate a 100 000-row export for no gain.
    return false;
}

}  // namespace

void append_csv_cell(std::string& out, std::string_view cell) {
    const bool formula = is_formula_lead(cell);
    // A neutralised cell always gets quoted as well: the apostrophe is the
    // spreadsheet's "treat as text" marker, and leaving it bare in a field that
    // also contains a comma would split the cell in two.
    if (!formula && !needs_quoting(cell)) {
        out.append(cell);
        return;
    }

    out.push_back('"');
    if (formula) { out.push_back('\''); }
    for (const char c : cell) {
        if (c == '"') { out.push_back('"'); }
        out.push_back(c);
    }
    out.push_back('"');
}

void append_csv_row_end(std::string& out) { out.append("\r\n"); }

}  // namespace anvil::http

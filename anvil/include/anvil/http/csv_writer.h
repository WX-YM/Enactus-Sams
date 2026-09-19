#pragma once

// CSV cell assembly for the submissions export (docs/13-dynamic-forms.md §5).
//
// Two hazards, both of which a naive `join(",")` walks straight into:
//
//   FORMULA INJECTION. A spreadsheet treats a cell beginning with
//   `=`, `+`, `-`, `@`, tab or CR as a formula, so a submitter's answer of
//   `=HYPERLINK("http://evil","click")` executes the moment a staff member
//   opens the export. The submitter chose that text and the staff member is
//   authenticated — this is stored code execution with a delivery mechanism
//   built into the product. Every such cell is prefixed with an apostrophe,
//   which spreadsheets read as "this is text".
//
//   ARABIC AS MOJIBAKE. Excel decodes a CSV in the system codepage unless the
//   file opens with a UTF-8 BOM. The bytes are correct either way; the file is
//   unreadable to the people it was exported for. The BOM is emitted once, at
//   the head of the file, and nowhere else.
//
// The quoting itself is RFC 4180: a cell containing a comma, a quote, CR or LF
// is wrapped in quotes and its own quotes are doubled. Note the ordering — the
// apostrophe goes INSIDE the quoting, because a cell that needs both must be
// neutralised as text and quoted as a field, and doing it the other way round
// produces `"'=..."` in some writers and `'"=..."` in others.

#include <cstdint>
#include <string>
#include <string_view>

namespace anvil::http {

inline constexpr std::string_view kCsvContentType = "text/csv; charset=utf-8";

// EF BB BF. Written once, as the first bytes of the file.
inline constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

// True when a spreadsheet would evaluate this cell rather than display it.
[[nodiscard]] constexpr bool is_formula_lead(std::string_view cell) noexcept {
    if (cell.empty()) { return false; }
    const char first = cell.front();
    return first == '=' || first == '+' || first == '-' || first == '@' || first == '\t' ||
           first == '\r';
}

// Appends one cell, neutralised and quoted as needed. Does NOT append a
// separator: the caller decides where the row ends, which is what keeps the
// last-column case from needing its own branch here.
void append_csv_cell(std::string& out, std::string_view cell);

// `\r\n`, per RFC 4180. Excel accepts a bare LF; Numbers and several older
// importers do not, and the two extra bytes a row cost nothing against the
// support burden of the alternative.
void append_csv_row_end(std::string& out);

}  // namespace anvil::http

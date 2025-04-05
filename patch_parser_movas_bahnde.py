import os

file_path = "src/parser/parser_movas_bahnde.cpp"

if not os.path.exists(file_path):
    print(f"Error: File not found at {file_path}. Please run this script from your project root.")
    exit(1)

with open(file_path, "r", encoding="utf-8") as f:
    lines = f.readlines()

updated_lines = []
for line in lines:
    # Fix enum class style usage (ParserAbstract::Mode::X -> X)
    if "ParserAbstract::Mode::" in line:
        line = line.replace("ParserAbstract::Mode::", "")
    
    # Also simplify type if needed (ParserAbstract::Mode -> int or keep as is)
    if "ParserAbstract::Mode" in line:
        line = line.replace("ParserAbstract::Mode", "int")

    # Fix Qt::CaseSensitivity usage
    if "Qt::CaseSensitivity::" in line:
        line = line.replace("Qt::CaseSensitivity::", "Qt::")

    updated_lines.append(line)

# Write changes back
with open(file_path, "w", encoding="utf-8") as f:
    f.writelines(updated_lines)

print("✅ Patch complete: parser_movas_bahnde.cpp updated.")


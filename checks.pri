# LLVM 18 checks, matching Oxide. Override these through qmake when needed.
isEmpty(EREADER_CLANG_FORMAT_EXECUTABLE): EREADER_CLANG_FORMAT_EXECUTABLE = clang-format-18
isEmpty(EREADER_CLANG_TIDY_EXECUTABLE): EREADER_CLANG_TIDY_EXECUTABLE = clang-tidy-18
isEmpty(EREADER_LINT_JOBS): EREADER_LINT_JOBS = 4
# Run make lint, make format, or make format-check in the configured build directory.
# EREADER_CLANG_TIDY_EXTRA_ARGS accepts --target=... and --gcc-toolchain=... for cross builds.

for(source, SOURCES): FORMAT_SOURCES += $$shell_quote($$absolute_path($$source, $$_PRO_FILE_PWD_))
for(header, HEADERS): FORMAT_SOURCES += $$shell_quote($$absolute_path($$header, $$_PRO_FILE_PWD_))

format.target = format
format.commands = $$shell_quote($$EREADER_CLANG_FORMAT_EXECUTABLE) --style=file -i $$FORMAT_SOURCES
format.depends = FORCE
format_check.target = format-check
format_check.commands = $$shell_quote($$EREADER_CLANG_FORMAT_EXECUTABLE) --style=file --dry-run --Werror $$FORMAT_SOURCES
format_check.depends = FORCE

# All plugin sources use the same qmake C++ flags. Pass them directly to
# clang-tidy so lint follows the selected SDK, definitions and private Qt headers.
for(source, SOURCES) {
    lint_target = lint_$$replace(source, [^A-Za-z0-9_], _)
    eval($${lint_target}.target = $$lint_target)
    eval($${lint_target}.commands = $$shell_quote($$EREADER_CLANG_TIDY_EXECUTABLE) $$shell_quote($$absolute_path($$source, $$_PRO_FILE_PWD_)) -- $(CXXFLAGS) $(INCPATH) $$EREADER_CLANG_TIDY_EXTRA_ARGS)
    eval($${lint_target}.depends = FORCE)
    QMAKE_EXTRA_TARGETS += $$lint_target
    lint_sources.depends += $$lint_target
}
lint_sources.target = lint-sources
lint.target = lint
lint.depends = compiler_moc_header_make_all compiler_moc_source_make_all FORCE
lint.commands = $(MAKE) -k -f $(MAKEFILE) -j$$EREADER_LINT_JOBS lint-sources

QMAKE_EXTRA_TARGETS += format format_check lint_sources lint

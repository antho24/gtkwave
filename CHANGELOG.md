# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Current limitations compared to GTKWave LTS 3.3

Some features have been removed in this version of GTKWave to make the codebase
easier to refactor. These features might be restored in future versions of GTKWave,
if there is enough interest by users. Please notify us on GitHub if you require any
of these features.

- Removed the interactive VCD mode.
- Removed support for LXT, LXT2, VZT, FSDB, and AET2 formats.
- Removed support compressed hierarchy names.
- Removed Tcl scripting support (will be replaced by Python scripting).

### Changed

- Merged in major GUI widget refactoring.
- Updated the UI design of many dialogs.
- Improved rendering of all 0s/1s states of vectors.
- Replaced hamburger menu with traditional menubar.
- Improved the readability of the cursor and marker time display.
- Migrated from autotools to meson.
- Changed the fallback text editor from gedit to the default editor that is associated with the source filetype.
- Changed file dialog to use the native dialog on all platforms.
- Changed regular expressions to use PCRE instead of POSIX syntax.

### Added

- Added support for `namespace import gtkwave::*` in Tcl scripts.
- Added OpenBSD and FreeBSD OS support for unbuffered FST I/O.
- Added `dbl_mant_dig_overrides` rc environment variable.
- Added `disable_antialiasing` rc variable.
- Added `editor_run_in_terminal` rc variable.
- VHDL records (in FST files written by NVC or GHDL and GHW files written by GHDL) are
  listed in the signals pane and are added as collapsible groups with nested records as
  nested groups. Arrays of records written by GHDL are added as a group of their
  elements. The group row of a record shows its composite value, e.g. `{1 05 run {0 3}}`.
  Records are shown as an indented tree with expanders that open and close the record
  with a single click.
- The signal names and values are shown in separate columns, each with its own width and
  horizontal scrollbar.
- The full hierarchical name of the signal under the pointer in the waveform or signal list
  is shown at the bottom of the main window.
- A marker bar below the waves shows the time of the primary, baseline and named markers
  and the time between neighboring markers. Markers can be dragged in the bar, a
  double-click adds a named marker and a right-click opens a menu to lock, rename or delete
  a marker. Dragged and added markers snap to the nearest edge of the visible signals or to
  another marker within the `cursor_snap` distance, Shift places them freely. The tooltip
  of a marker shows the time to every other marker. The bar can be hidden with
  View → Show Marker Bar.
- Signals of the VHDL numeric types are shown in the radix of their type when they are added:
  `signed` and integer signals as signed decimal, `unsigned` as decimal and `ufixed` and
  `sfixed` as fixed point numbers (e.g. `-1.25`). The types are taken from the type names in
  FST files written by NVC and from GHW files (GHDL writes no types to FST files). The
  `vhdl_type_format` rc variable turns this off.
- VHDL arrays are listed in the signals pane and added as collapsible groups like records,
  with their elements named `[0]`, `[1]`, ... and the composite value in the group row.
  Two-dimensional arrays (in GHW files) are added as nested groups, one per row. The
  elements of arrays that NVC writes as separate signals are grouped into an array in the
  hierarchy.
- Vectors in GHW files are listed in the signals pane of their scope, e.g. `u[7:0]`.
- Named markers can be locked in the marker bar or in the named markers dialog. Locked
  markers are drawn with a solid line and a padlock, can't be dragged or collected and are
  stored in save files (`[markerlock]`).
- Bookmarks for time ranges: Time → Bookmarks → Add Bookmark (Ctrl+Shift+B) bookmarks the
  visible time range, the Bookmarks menu shows a bookmark again without moving the markers
  and Manage Bookmarks renames and deletes them. Bookmarks are stored in save files
  (`[bookmark]`).

### Removed

- Removed support for GTK 2.
- Removed splash screen.
- Removed "Signal Search Hierarchy" dialog.
- Removed setting to disable the "Alternate Wheel Mode".
- Removed legacy VCD loader.
- Removed VCD fastload and support for vlist spillfiles.
- Removed VCD autosave.
- Removed rc variables:
    - `alt_hier_delimeter`
    - `alt_wheel_mode`
    - `append_vcd_hier`
    - `atomic_vectors`
    - `disable_auto_comphier`
    - `disable_ae2_alias`
    - `enable_vcd_autosave`
    - `force_toolbars`
    - `hide_sst`
    - `hier_grouping`
    - `hpane_pack`
    - `lxt_clock_compress_to_z`
    - `max_fsdb_trees`
    - `splash_disable`
    - `use_scrollbar_only`
    - `use_scrollwheel_as_y`
    - `use_standard_clicking`
    - `use_standard_trace_select`
    - `use_toolbutton_interface`
    - `vcd_explicit_zero_subscripts`
    - `vlist_spill`
    - `zoom_dynamic`
    - `zoom_dynamic_end`

### Fixed

- Fixed toggle menu item access under Tcl.
- Path fix for `twinwave` on Windows.
- Fixed high CPU usage on Wayland.
- Fixed display update issues on Wayland.
- Fixed slow redrawing of dense waveforms.
- Compiler fix for `__GTK_SOCKET_H__` availability.
- Fixed the `++` and `--` direction filters of the signals pane, which matched linkage
  signals instead of signals without a direction.
- Fixed Markers → Locking (Q, W and O), which aborted GTKWave. Moving the primary marker
  again moves the named marker it is attached to.
- Fixed marker aliases, which were saved as the name of the marker.
- Fixed the named markers dialog, which removed markers before the visible time range.
- Fixed enumerations in FST files written by GHDL, which were named with a bit range
  (`st[7:0]`).
- Fixed arrays of enumerations and booleans in FST files written by NVC, whose elements lost
  their index so that only one element was shown.
- Fixed the data types of FST signals with a VHDL type name (e.g. `real` signals had the
  data type `integer`).
- Fixed save files, which enabled the named markers that were never set at time 0.

[Unreleased]: https://github.com/gtkwave/gtkwave/compare/v3.3.116...HEAD

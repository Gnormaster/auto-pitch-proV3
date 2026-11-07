Auto-Pitch Pro - VST2 (64 bit) mit Original-Oberflaeche

Erzeugen der DLL:
  build_vst2.bat doppelt anklicken (benoetigt Visual Studio mit C++-Workload und Internet
  fuer den einmaligen Download des WebView2-SDK).
  Ergebnis: AutoPitchPro.dll im selben Ordner (einzelne Datei, Oberflaeche ist eingebettet).

Installation:
  AutoPitchPro.dll in den VST-Ordner von FL Studio kopieren,
  danach Options > Manage plugins > Find installed plugins.

Voraussetzung auf dem Rechner: Microsoft Edge WebView2 Runtime (in Windows 11 enthalten).

Alternative ohne lokales Bauen:
  Dateien in ein GitHub-Repository hochladen (inklusive .github), der Workflow
  "Build VST2 DLL" legt AutoPitchPro.dll unter Releases ab.

Parameter: 26 (Retune Speed, Flex Tune, Humanize, Natural Vibrato, Key, Scale, Input Type,
Tracking, Transpose, Detune, Formant, Mix, Classic Mode, Harmony On, H1-H4 Interval/Level/Pan).
Latenz je nach Input Type 22 bis 60 ms, wird dem Host gemeldet.

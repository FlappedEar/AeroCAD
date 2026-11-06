# AeroCAD

Natywna aplikacja CAD (Windows 11 x64, macOS Apple Silicon) do projektowania części
na bazie dowolnego modelu 3D:

1. Wczytujesz plik 3D (STL, OBJ, 3MF, STEP, ...).
2. Oglądasz model: obrót, przesuwanie, przybliżanie.
3. Zaznaczasz obszar na modelu, a aplikacja tworzy na nim powierzchnię bazową.
4. Projektujesz na niej narzędziami CAD (szkic, wyciągnięcie, shell, fillet, boolean, ...).
5. Eksportujesz wynik do STEP, STL lub 3MF.
6. Osobno uruchamiasz symulację CFD i dostajesz wykresy.
7. Agent AI (przez MCP) może wykonywać te same operacje co użytkownik.

Status: wczesny szkielet (etap 0.1, epik CAD-1 „Fundament”).

Stos: C++20, Qt 6 (Qt Quick, Qt Quick 3D), Open CASCADE Technology, CMake.

Licencja: GPL-3.0-or-later (plik [LICENSE](LICENSE)).

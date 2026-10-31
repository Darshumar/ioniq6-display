// Ablage vor den Getraenkehaltern (Ioniq 6), nachgebaut aus Marcs Massen vom 23.10.
// Nur zur Anschauung, nicht zum Drucken. Koordinaten wie beim Staender:
// y = 0 Kante zu den Getraenkehaltern, nach vorne (Armaturenbrett) negativ.
use <ws43_akkufach.scad>

show = "alles";     // ablage | alles

A = 135;   // Gesamtbreite innen
B = 97;    // linke Flaeche
C = 115;   // Tiefe bis unter das Klimabedienteil
H = 33;    // Seitenwaende
E = 15;    // Kante zu den Getraenkehaltern
D = 15;    // Erhoehung
flat = 23; ramp = 45;  // ebener Teil ab Kante, Rampe (aus Fotos geschaetzt)
F = 55;    // Platz unter dem Klimabedienteil
wall = 6;
T = 16;    // Mulde tiefer als die linke Flaeche (Marc 27.10. gemessen)

// x = 0 Trennkante; linke Flaeche (vom Fahrer aus) bei x > 0, USB-Nische bei x < 0
module ablage() {
    G2 = A - B;
    color("DimGray") rotate([90, 0, 90]) linear_extrude(B)
        polygon([[0, -3], [0, 0], [-flat, 0], [-flat - ramp, D], [-C, D], [-C, -3]]);
    color("Gray") translate([-G2, -C, -3 - T]) cube([G2, C, 3]);
    color("DimGray") translate([-0.01, -C, -3 - T]) cube([0.02, C, T + 3]);
    // Mulde liegt T tiefer als die linke Flaeche
    // USB-Buchse in der Nische (Lage grob nach Foto)
    color("Black") translate([-G2 + 10, -C + 35, 8 - T]) cube([16, 4, 14]); // Hoehe grob
    color("SlateGray") {
        translate([B, -C, -3]) cube([wall, C, H + 3]);
        translate([-G2 - wall, -C, -3 - T]) cube([wall, C, H + 3 + T]);
        translate([-G2 - wall, 0, -3]) cube([A + 2 * wall, 12, E + 3]);
    }
    // Klimabedienteil (Ueberstand geschaetzt 20 mm)
    color("Black", 0.85) translate([-G2 - wall, -C - 10, F]) cube([A + 2 * wall, 30, 8]);
    color("Black", 0.85) translate([-G2 - wall, -C - 14, -3]) cube([A + 2 * wall, 4, F + 40]);
}

ablage();
if (show == "alles") stand_preview_only();

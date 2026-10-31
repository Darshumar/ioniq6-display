// Akkufach fuer das Gehaeuse des Waveshare ESP32-S3 Touch-LCD 4.3" (800x480)
//
// Standard (variante="einteilig"): Marcs Wanne (stl/ws43_tray_original.stl) wird
// um Akkudicke + Luft tiefer, der Akku liegt unter dem Board. Vier M2,5-Zylinderkopf-
// schrauben gehen von unten durch hohe Bolzen in die Abstandsbolzen des Boards.
//
//   openscad -D 'part="tray_gross"' -o stl/ws43_tray_akku.stl ws43_akkufach.scad
//   openscad -D 'part="staender"'  -o stl/ws43_staender_akku.stl ws43_akkufach.scad
//   openscad -D 'part="preview"'   -o preview.png ws43_akkufach.scad

part = "preview";   // tray_gross | staender | preview | stand_preview | akkuwanne | tray | explode

// einteilig: eine tiefere Wanne, Akku liegt direkt unter dem Board (Standard)
// zweiteilig: Originalwanne + separate Akkuwanne darunter
variante = "einteilig";

// --- Akku: Meshnology 103665, 3,7 V 3000 mAh, PH2.0 (Amazon B0FJ1GX4P6) ---
// Nennmass 10 x 36 x 65 mm; Laenge inkl. Schutzplatine und Toleranz zugegeben
batt_l = 69;        // Laenge
batt_w = 37;        // Breite
batt_t = 10.5;       // Dicke
batt_clear = 1.5;   // Luft ueber dem Akku (Schaumstoff/Klebepad)
batt_ribs = true;   // Halterippen um den Akku
rib_h = 3;
batt_x = 46;        // Mitte des Akkus (Koordinaten der Originalwanne)
batt_y = 34;

// --- Originalwanne (gemessen aus der STL) ---
o_min = [-2.4, -2.4];
o_max = [108.5, 70.2];
o_r   = 1.0;                // Eckradius aussen
tray_h = 15;
holes = [[4.03, 3.97], [102.11, 3.97], [4.03, 63.84], [102.11, 63.84]];

// --- Akkuwanne ---
wall  = 2.0;
floor_t = 2.0;
tube_d = 8;
// M2,5 statt M3 (Marc 27.10.: M3 passt nicht ins Board)
screw_d = 2.9;
// Senkbohrung unten fuer M2,5-Zylinderkopf (ISO 4762, Kopf 4,5 x 2,5)
cb_d = 5.0;
cb_h = 2.7;
box_h = floor_t + batt_t + batt_clear;
// einteilig: so viel hoeher wird die Wanne (Akku + Luft bis zur alten Bodenoberkante)
dz = batt_t + batt_clear;
extra_h = variante == "einteilig" ? dz : box_h;

// --- Kabelloch im Wannenboden (Akkukabel zum Board) ---
slot = [12, 6];             // Laenge x Breite
slot_pos = [90, 34];        // Mitte

// --- Staender (nachgebaut nach Marcs Original-Staender) ---
// Schlitz 20 Grad nach hinten geneigt; Original: Schlitz 15,4 mm fuer 15 mm Gehaeuse,
// 116,1 x 40 x 15 mm. Hier waechst der Schlitz um extra_h.
// Einsatz fuer die linke Flaeche der Ablage vor den Getraenkehaltern (Marc, 23.10.):
// Flaeche B = 97 mm breit, Seitenwaende 33 mm hoch. Der Staender klemmt zwischen
// linker Wand und USB-Nische; das Gehaeuse (111 mm) ragt rechts ueber die Nische,
// deshalb ist der Schlitz rechts offen. Die USB-Seite des Gehaeuses liegt dabei
// rechts, also zur USB-Buchse des Autos hin.
st_len = 96;                // Original 116,1
st_r = 1.0;
// Boden der Ablage: vorne 23 mm eben (ab Kante Getraenkehalter), dann steigt er
// um 15 mm an. Der Staender steht vorne auf dem tiefen Teil und hinten auf der
// Erhoehung; die Unterseite ist entsprechend ausgespart. Laenge der Schraege
// ist geschaetzt (st_ramp).
st_flat = 23;
st_lift = 15;
st_ramp = 45;               // waagerechte Laenge der Rampe, aus Fotos geschaetzt (23.10.)
st_h = 15 + st_lift;        // 15 mm ueber der Erhoehung (Marc: 5 mm niedriger)
st_tilt = 35;               // Neigung nach hinten (Original 20, Marc 23.10.: 35 flacher)
// vom Fahrer aus gesehen liegt x = 0 rechts (USB-Nische), x = st_len links (Seitenwand)
st_slot_x = [-1, st_len + 1];   // beidseitig offen (Marc: kein Anschlag links)
case_left = st_len;         // Vorschau: linke Gehaeusekante buendig mit dem Staender
st_slot_w = tray_h + 0.4 + extra_h;
st_lip_wall = 8;            // Wandstaerke vor dem Schlitz oben (Original 4,1)
st_back_wall = 19.4;        // Material hinter dem Schlitz oben
st_lip = 7;                 // senkrechte Hoehe der Vorderlippe ueber dem Schlitzboden
// tiefster Punkt des Schlitzes, so gelegt, dass die Vorderlippe st_lip hoch bleibt
st_floor = st_h - st_lip - st_slot_w * sin(st_tilt);
// Nase gegen seitliches Rutschen (Marc 24.10.): Rippe an der hinteren
// Schlitzwand, greift in eine Nut in der Wannenrueckseite (unten offen,
// das Gehaeuse wird von oben eingesetzt)
// Fluegel in die USB-Mulde (Marc 26.10.): flache Platte rechts unter dem
// Staender quer ueber die Mulde bis an deren rechte Wand, damit sich der
// Staender dort abstuetzt und nicht nach rechts rutscht. Der Schlitz bleibt.
wing_w = 37;                // Breite nach rechts (Mulde A - B = 38 mm, 1 mm Luft)
wing_d = 35;                // Tiefe ab Kante Getraenkehalter (endet vor dem USB-Gehaeuse)
wing_t = 6;                 // Oberkante linker Schenkel vorne ueber der ebenen Flaeche (= Zapfenhoehe)
wing_top_end = -12;         // Oberkante hinten, unter der ebenen Flaeche: faellt zum USB ab
wing_drop = 16;             // Mulde tiefer als die ebene Flaeche (Marc 27.10. gemessen)
wing_wall = 2.5;            // Wandstaerke des U-Profils (Marc 26.10.: U in der Mulde statt massiv)
wing_right = 8;             // Hoehe des rechten Schenkels ueber dem Muldenboden
wing_clear = 0.3;
// Steckverbindung (Marc 26.10.: Fluegel getrennt drucken): Schwalbenschwanz-
// Zapfen am linken Schenkel, Nut im Staender unten offen; Fluegel in die Mulde
// legen, Staender von oben aufsetzen
dt_y = -12;                 // Mitte des Zapfens (y)
dt_neck = 6;                // Breite am Hals (x = 0)
dt_wide = 10;               // Breite am Ende
dt_len = 6;                 // Laenge in den Staender (x)
dt_fit = 0.25;              // Spiel je Seite           // Luft zur Rampe hinter der ebenen Flaeche
nose_X = 48;                // Lage im Staender (x), Gehaeuse buendig links
nose_w = 3;                 // Breite der Rippe
nose_h = 1.0;               // Hoehe ueber der Schlitzwand
nose_l = 14;                // Laenge an der Wand ab Schlitzboden
groove_clear = 0.4;         // Luft der Nut ringsum
groove_x = case_left + o_min[0] - nose_X;   // Lage der Nut in Wannenkoordinaten

$fn = 48;

module rrect(a, b, r) {
    translate([a[0] + r, a[1] + r])
        offset(r = r) square([b[0] - a[0] - 2 * r, b[1] - a[1] - 2 * r]);
}

module akkuwanne() {
    difference() {
        union() {
            difference() {
                linear_extrude(box_h) rrect(o_min, o_max, o_r);
                translate([0, 0, floor_t])
                    linear_extrude(box_h)
                        rrect(o_min + [wall, wall], o_max - [wall, wall], 0.5);
            }
            for (h = holes) translate(h) cylinder(d = tube_d, h = box_h);
            if (batt_ribs) translate([batt_x, batt_y, floor_t]) ribs();
        }
        for (h = holes) translate(h) {
            translate([0, 0, -1]) cylinder(d = screw_d, h = box_h + 2);
            translate([0, 0, -0.01]) cylinder(d = cb_d, h = cb_h + 0.01);
        }
    }
}

// L-Winkel an den vier Ecken des Akkus, 0,5 mm Luft
module ribs() {
    for (sx = [-1, 1], sy = [-1, 1]) corner(sx, sy);
}

module corner(sx, sy) {
    c = 0.5; t = 1.6; len = 8;
    lx = batt_l / 2 + c; ly = batt_w / 2 + c;
    translate([sx * lx, sy * ly, 0]) mirror([sx < 0 ? 1 : 0, 0, 0]) mirror([0, sy < 0 ? 1 : 0, 0]) {
        translate([0, -len, 0]) cube([t, len + t, rib_h]);
        translate([-len, 0, 0]) cube([len + t, t, rib_h]);
    }
}

module tray() {
    difference() {
        import("stl/ws43_tray_original.stl", convexity = 6);
        translate([slot_pos[0], slot_pos[1], -1])
            linear_extrude(5) offset(r = 1) square(slot - [2, 2], center = true);
    }
}

// Profil in der y-z-Ebene, Vorderkante bei y = 0, nach hinten negativ
function st_pts() = let(
    t = tan(st_tilt), c = cos(st_tilt), sn = sin(st_tilt),
    zF = st_floor + st_slot_w * sn,
    F = [-st_lip_wall + (st_h - zF) * t, zF],
    P = F - st_slot_w * [c, sn],
    B = [P[0] - (st_h - P[1]) * t, st_h],
    L = [-st_lip_wall, st_h]
) [B, P, F, L];

function st_depth() = -st_pts()[0][0] + st_back_wall;

// Schwalbenschwanz im Grundriss, von x = -wing_clear - 1 bis dt_len
module dovetail(f = 0) {
    polygon([[-wing_clear - 1, dt_y - dt_neck / 2 - f], [0, dt_y - dt_neck / 2 - f],
             [dt_len + f, dt_y - dt_wide / 2 - f], [dt_len + f, dt_y + dt_wide / 2 + f],
             [0, dt_y + dt_neck / 2 + f], [-wing_clear - 1, dt_y + dt_neck / 2 + f]]);
}

// Fluegel in die USB-Mulde, eigenes Druckteil (Koordinaten wie der Staender)
module fluegel() {
    // U-Profil auf dem Muldenboden (wing_drop unter der Grundflaeche); hinter
    // der ebenen Flaeche faellt die Oberkante zum USB-Port ab; zur Stufe wing_clear Luft
    intersection() {
        // Boden, rechter Schenkel an der Muldenwand, linker Schenkel an der Stufe
        union() {
            translate([-wing_w, -wing_d, -wing_drop]) cube([wing_w - wing_clear, wing_d, wing_wall]);
            translate([-wing_w, -wing_d, -wing_drop]) cube([wing_wall, wing_d, wing_right]);
            translate([-wing_clear - wing_wall, -wing_d, -wing_drop]) cube([wing_wall, wing_d, wing_drop + wing_t]);
        }
        hull() {
            translate([-wing_w - 1, -st_flat, -wing_drop - 1]) cube([wing_w + 4, st_flat + 1, wing_drop + 1 + wing_t]);
            translate([-wing_w - 1, -wing_d - 1, -wing_drop - 1]) cube([wing_w + 4, 1, wing_drop + 1 + wing_top_end]);
        }
    }
    // Zapfen auf dem linken Schenkel
    linear_extrude(wing_t) dovetail();
}

module staender() {
    p = st_pts();
    d = st_depth();
    sn = sin(st_tilt); c = cos(st_tilt);
    u = [-sn, c]; n = [c, sn];
    // Rippe, oben angeschraegt als Einfuehrung
    translate([nose_X - nose_w / 2, 0, 0]) rotate([90, 0, 90]) linear_extrude(nose_w)
        polygon([p[1] - n, p[1] + nose_l * u - n, p[1] + nose_l * u,
                 p[1] + (nose_l - 2) * u + nose_h * n, p[1] + nose_h * n]);
    difference() {
        translate([0, -d, 0]) linear_extrude(st_h) rrect([0, 0], [st_len, d], st_r);
        translate([st_slot_x[0], 0, 0]) rotate([90, 0, 90])
            linear_extrude(st_slot_x[1] - st_slot_x[0])
                polygon([p[0] + [0, 1], p[1], p[2], p[3] + [0, 1]]);
        // Nut fuer den Zapfen des Fluegels, unten und rechts offen
        translate([0, 0, -1]) linear_extrude(wing_t + 1 + 0.4) dovetail(dt_fit);
        // Aussparung fuer die Erhoehung hinten (0,3 mm Luft)
        c = 0.3;
        translate([-1, 0, 0]) rotate([90, 0, 90]) linear_extrude(st_len + 2)
            polygon([[-st_flat - c, -1], [-st_flat - c, 0],
                     [-st_flat - c - st_ramp, st_lift + c],
                     [-d - st_ramp - 1, st_lift + c], [-d - st_ramp - 1, -1]]);
    }
}

// Einteilige Wanne aus Marcs Original zusammengesetzt, damit Kanten und Radien
// gleich bleiben: unten der Originalboden (0..1 mm, mit Kantenrundung und
// Senkungen), dann der Wandquerschnitt des Originals (bei z = 8) um dz
// verlaengert, oben der Rest des Originals (ab z = 1) um dz angehoben, dessen
// Boden entfernt. Die Board-Schrauben gehen von unten durch hohe Bolzen, deren
// Oberkante auf Hoehe der alten Bodenoberkante liegt.
module orig() { import("stl/ws43_tray_original.stl", convexity = 6); }
module orig_wall() { projection(cut = true) translate([0, 0, -8]) orig(); }
module inner2d() { rrect([-0.4, -0.4], [106.5, 68.2], 0.3); }

// Zusaetzliche Oeffnungen (Marc 23.10., Mass + Foto; z wie im Original, ohne dz)
// Das Original hat rechts nur die Oeffnung fuer UART-USB-C (Mitte y 55,05,
// z 4,6, 9,4 x 3,6). Die USB-Buchse (nativ) sitzt 11 mm Luecke weiter vorne,
// laut Foto ca. 21 mm Mitte-Mitte. Probedruck 24.10.: Buchse sass 1,1 mm
// weiter Richtung UART als angenommen, Oeffnung jetzt so breit wie das Original.
usb2_y = 34.6;  usb2_w = 9.8;  usb_h = 3.6;  usb_z = 4.6;
// BOOT/RESET an der oberen Kante (y-max), Taster 6 mm, 5 mm Abstand;
// Probedruck 24.10.: beide Taster ca. 1 mm weiter Richtung USB-Kante als
// zuerst angenommen (BOOT Mitte ca. 49 mm von der rechten Platinenkante x 106,1)
btn_x = [57.0, 46.5];
btn_w = 8.5;  btn_h = 4.5;  btn_z = 4.6;
// microSD an derselben Kante, nahe der linken Ecke (Foto 23.10., Karte 11 mm
// breit, Mitte ca. 22,7 mm von der linken Platinenkante; Offset wie bei den
// Tasten korrigiert). Breiter als die Karte, damit ein Fingernagel hinkommt.
sd_x = 22.7;  sd_w = 14;  sd_h = 4;  sd_z = 5.0;
module stadium(w, h, len) {
    hull() for (s = [-1, 1]) translate([s * (w - h) / 2, 0, 0]) cylinder(d = h, h = len);
}
// Tastenkappen fuer BOOT/RESET (Marc 26./27.10.), passen ohne Aenderung in die
// Oeffnungen. Erste Version mit Kopf aussen fiel heraus, daher mit Kragen.
cap_fit = 0.25;             // Spiel je Seite in der Oeffnung
cap_tip = [3, 2.5];         // Stoessel am Ende
// Kappe mit Kragen (Marc 27.10.: kurze Kappe passt, faellt bei schraeg
// stehendem Gehaeuse aber heraus). Wird von innen eingesetzt, bevor das Board
// eingeschraubt wird. Kein Kopf: der Stift ragt aussen 1,5 mm heraus. Der Kragen
// sitzt hinter der Wand nur an den Enden (neben dem 6 mm breiten Taster).
// Mit der kurzen Kappe (2,6) lag der Stoessel hoechstens ca. 0,6 mm hinter der
// Wand; Probedruck 27.10.: Mitte buendig mit der Wand-Innenseite passt.
kcap_out = 1.5;             // Ueberstand aussen
kcap_wall = 2.0;            // Wandstaerke der Wanne
kcap_collar = [9.4, 3.0, 0.5];  // Kragen: Breite, Hoehe, Staerke
kcap_free = 3.2;            // halbe Breite ohne Kragen (Taster 6 mm)
kcap_tips = [0.0];          // Stoessel hinter der Wand-Innenseite (Probedruck 27.10.: 0 passt; 0,2/0,4 getestet)
module tastenkappe_kragen(tip) {
    // z = 0 Aussenseite der Wand, innen negativ
    w = btn_w - 2 * cap_fit;  h = btn_h - 2 * cap_fit;
    hull() {
        translate([0, 0, -kcap_wall]) stadium(w, h, kcap_wall + kcap_out - 0.5);
        translate([0, 0, -kcap_wall]) stadium(w - 1, h - 1, kcap_wall + kcap_out);
    }
    translate([0, 0, -kcap_wall - tip]) stadium(cap_tip[0], cap_tip[1], tip + 0.01);
    translate([0, 0, -kcap_wall - kcap_collar[2]]) difference() {
        stadium(kcap_collar[0], kcap_collar[1], kcap_collar[2] + 0.01);
        translate([-kcap_free, -5, -1]) cube([2 * kcap_free, 10, 5]);
    }
}
// Druckbild: Aussenseite nach unten, je Variante zwei Kappen (BOOT, RESET);
// bei mehreren Varianten zaehlen Kerben am Rand die Variante
module kappen_kragen_satz() {
    for (i = [0 : len(kcap_tips) - 1], j = [0, 1])
        translate([j * 13, i * 9, 0]) rotate([180, 0, 0]) translate([0, 0, -kcap_out])
            difference() {
                tastenkappe_kragen(kcap_tips[i]);
                if (len(kcap_tips) > 1) for (k = [0 : i]) translate([-1.6 + k * 1.2, btn_h / 2 - cap_fit - 0.4, -0.3])
                    cube([0.6, 2, 2]);
            }
}

module side_openings() {
    translate([103, usb2_y, dz + usb_z]) rotate([0, 90, 0]) rotate([0, 0, 90])
        stadium(usb2_w, usb_h, 10);
    for (x = btn_x) translate([x, 65, dz + btn_z]) rotate([-90, 0, 0])
        stadium(btn_w, btn_h, 10);
    translate([sd_x, 65, dz + sd_z]) rotate([-90, 0, 0]) stadium(sd_w, sd_h, 10);
}

module tray_gross() {
    split = 1.0;
    post_top = dz + 1.9;
    difference() {
        union() {
            intersection() { orig(); translate([-10, -10, 0]) cube([130, 90, split]); }
            translate([0, 0, split]) linear_extrude(dz + 0.01) orig_wall();
            translate([0, 0, split - 0.01]) linear_extrude(floor_t - split + 0.01) inner2d();
            translate([0, 0, dz]) difference() {
                intersection() { orig(); translate([-10, -10, split]) cube([130, 90, 20]); }
                translate([0, 0, split - 0.1]) linear_extrude(1.95 - split + 0.1) inner2d();
            }
            for (h = holes) translate(h) cylinder(d = tube_d, h = post_top);
            if (batt_ribs) translate([batt_x, batt_y, floor_t]) ribs();
        }
        side_openings();
        // Nut fuer die Nase des Staenders, unten offen
        translate([groove_x - nose_w / 2 - groove_clear, -5, -1])
            // Rueckseite liegt 0,2 mm vor der Wand: Nut 1,1 tief, 0,9 mm Boden bleibt
            cube([nose_w + 2 * groove_clear, 5 + nose_l - 2.4 + 1, 1 + nose_h - 0.2 + 0.3]);
        for (h = holes) translate(h) {
            translate([0, 0, -1]) cylinder(d = screw_d, h = post_top + 2);
            translate([0, 0, -0.01]) cylinder(d = cb_d, h = cb_h + 0.01);
        }
    }
}

// Staender mit Gehaeuse quer im Schlitz, Rueckseite (Akku) an der hinteren Schlitzwand
module stand_preview_only() {
    color("DimGray") staender();
    color("Orange") fluegel();
    case_in_stand() case_assembled();
}
// Lage des Gehaeuses im Staender-Schlitz
module case_in_stand() {
    p = st_pts();
    sn = sin(st_tilt); c = cos(st_tilt);
    o = p[1] + 2.4 * [-sn, c] + 0.2 * [c, sn];
    multmatrix([[-1, 0, 0, case_left + o_min[0]],
                [0, -sn, c, o[0]],
                [0, c, sn, o[1]],
                [0, 0, 0, 1]]) children();
}

show_display = true;

// Display (Glas oben buendig mit der Wanne), nur fuer Vorschauen
module display_dummy(top) {
    color("Black") translate([-0.2, -0.2, top - 1.6]) cube([106.5, 68.2, 1.6]);
    color("MidnightBlue") translate([5.5, 6.5, top - 0.05]) cube([95, 54, 0.1]);
    color("DeepSkyBlue") translate([12, 40, top]) cube([30, 10, 0.1]);
    color("LimeGreen") translate([12, 18, top]) cube([55, 8, 0.1]);
    color("Orange") translate([75, 18, top]) cube([18, 32, 0.1]);
}

module battery() {
    color("DarkSlateGray") translate([batt_x - batt_l / 2, batt_y - batt_w / 2, floor_t])
        cube([batt_l, batt_w, batt_t]);
}

module board_dummy() {
    color("Black") translate([-0.2, -0.2, box_h + 4]) cube([106.5, 68, 10.8]);
}

module case_assembled() {
    if (variante == "einteilig") {
        color("SteelBlue") tray_gross();
        battery();
        if (show_display) display_dummy(dz + tray_h);
    } else {
        color("SteelBlue") akkuwanne();
        battery();
        color("LightSteelBlue") translate([0, 0, box_h]) tray();
    }
}

if (part == "tray_gross") tray_gross();
else if (part == "cutaway") {
    difference() {
        union() { color("SteelBlue") tray_gross(); battery(); }
        translate([-5, -10, -1]) cube([120, 44, 40]);
    }
    color("Black", 0.5) translate([-0.2, 34, dz + 1.9]) cube([106.5, 34, 1.6]);
}
else if (part == "akkuwanne") akkuwanne();
else if (part == "staender") staender();
else if (part == "fluegel") fluegel();
else if (part == "kappen_kragen") kappen_kragen_satz();
else if (part == "stand_preview") {
    // Boden der Ablage (Kante Getraenkehalter bei y = 0, 15 mm hoch)
    color("Tan", 0.6) translate([-20, 0, 0]) rotate([90, 0, 90]) linear_extrude(150)
        polygon([[12, -3], [12, 15], [0, 15], [0, 0], [-st_flat, 0],
                 [-st_flat - st_ramp, st_lift], [-90, st_lift], [-90, -3]]);
    stand_preview_only();
}
else if (part == "tray") tray();
else if (part == "explode") {
    color("SteelBlue") akkuwanne();
    battery();
    color("LightSteelBlue", 0.9) translate([0, 0, box_h + 25]) tray();
} else {
    color("SteelBlue") akkuwanne();
    battery();
    color("LightSteelBlue") translate([0, 0, box_h]) tray();
}

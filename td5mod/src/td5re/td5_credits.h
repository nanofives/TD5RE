/* td5_credits.h — generated from English Language.dll SNK_CreditsText @RVA 0x7ad0
 * (re/tools/extract_credits.py). 24-byte-stride array of credit rows:
 *   "#X"  = a mugshot photo block; the letter X indexes the dev-photo table
 *           (A=Bob .. V=Daz, matching the original LoadTga order).
 *   other = a centered credit text line (" " = blank spacer row).
 * The original ScreenExtrasGallery (0x417d50) composites these into a vertical
 * scroll reel (screen x=0xcc, reel w=0x140=320, ~1 row / 32 frames). */
#ifndef TD5_CREDITS_H
#define TD5_CREDITS_H

static const char *const k_credits[] = {
    "TEST DRIVE 5", " ", "(C) ACCOLADE", "1998", " ", " ", " ", "DEVELOPED BY", "THE PITBULL SYNDICATE", " ",
    " ", " ", " ", " ", "PROGRAMMING", " ", " ", "#A", "BOB TROUGHTON", " ", " ", " ", "#B", "GARETH BRIGGS",
    " ", " ", " ", "#C", "STEVE SNAKE", " ", " ", " ", " ", " ", "ADDITIONAL", "PROGRAMMING", " ", " ", "#D",
    "MICHAEL TROUGHTON", " ", " ", " ", "#E", "CHRIS KIRBY", " ", " ", " ", "#F", "HEADLEY LEMARR", " ", " ",
    " ", " ", " ", "3D ART", " ", " ", "#G", "STEVE DIETZ", " ", " ", " ", "#U", "JONATHAN F. KAY", " ", " ",
    " ", "#H", "RICHARD MCDONALD", " ", " ", " ", "#I", "MIKE PIRSO", " ", " ", " ", "#J", "RICHARD BESTON",
    " ", " ", " ", " ", " ", "2D ART", " ", " ", "#K", "LES BURNEY", " ", " ", " ", "#L", "TONY PRINGLE", " ",
    " ", " ", "#M", "JOHN STEELE", " ", " ", " ", "#N", "DAVID TAYLOR", " ", " ", " ", " ", " ",
    "AI AND TESTING", " ", " ", "#V", "DAZ KELLY", " ", " ", " ", "#O", "TONY CHARLTON", " ", " ", " ", " ",
    " ", "STUDIO MANAGER", " ", " ", "#P", "DAVID BURTON", " ", " ", " ", " ", " ", "EXECUTIVE PRODUCER", " ",
    " ", "#Q", "CHRIS DOWNEND", " ", " ", " ", " ", " ", "PRODUCER", " ", " ", "#R", "SLADE ANDERSON", " ",
    " ", " ", " ", " ", "ASSOCIATE PRODUCER", " ", " ", "#S", "MATTHEW GUZENDA", " ", " ", " ", " ", " ",
    "LEAD QA ANALYST", " ", " ", "#T", "MARIE PERSON", " ", " ", " ", " ", " ", " ", "SPECIAL THANKS:", " ",
    " ", "RAYMOND OF FEAR", "FACTORY", " ", "AUXY", "DOLLY", "EMMA PALMER", "PETER HAYNES", "GARETH PUGH",
    "BEN SAMUELSON", "JASON LORD", "MARTIN GRIFFITHS", "COLIN ROBINSON", "AND TONY AT HEXHAM",
    "HORSELESS CARRIAGES.", " ", " ", "ACCOLADE CREDITS:", " ", " ", "ASSISTANT PRODUCER", "SEAN FISH ", " ",
    "SENIOR BRAND", "MARKETING MANAGER", "STEVE ALLISON", " ", "SOUND EFFECTS", "TOMMY TALLARICO", "STUDIOS",
    " ", "INTRO CINEMATIC", "PRODUCER", "STEVE ALLISON", " ", "QA MANAGER", "BRIAN GILMER", " ",
    "QA ANALYSTS", " ", "JASON LEVAN,", "DONNY CLAY,", "JASON CORDERO,", "ARIF SINAN,", "JAMES STRAWN,",
    "MOYE DANIEL,", "AND GREG REIMCHE. ", " ", "COMPATIBILITY TEST", "SUPERVISOR", "DAVID ABRAMS", " ",
    "COMPATIBILITY", "ANALYSTS", " ", "CHRISTOPHER D. REIMER,", "ADAM STOKKE", " ", "MUSIC SOUND TRACK",
    "COMPILED BY", "STEVE ALLISON", " ", "LICENSING MANAGER", "GABRIELLE BENHAM", " ", "LICENSING AND MEDIA",
    "SPECIALIST", "CHRISTINE LUGTON", " ", "INTERNATIONAL LIAISON", "JASON COHEN", " ", "CONSULTANT",
    "JEFF TAWNEY", " ", "DOCUMENTATION", "W.D. ROBINSON", " ", "WEB SITE DEVELOPMENT", " ", "RAY MASSA",
    "DANIEL GROVE", " ", "MARKETING SERVICES", " ", "MATT ABRAMS", "MARK GLOVER", "JILL DOS SANTOS", " ",
    "USA MASTERING", "SUPERVISOR", "LUIS RIVAS", " ", "TOOLS PROGRAMMING", "ERIC TETZ", " ", "SPECIAL THANKS",
    " ", "JIM BARNETT", "STAN ROACH", "NEIL JOHNSTON", "ERICA KRISHNAMURTHY",
};
#define K_CREDITS_COUNT ((int)(sizeof(k_credits)/sizeof(k_credits[0])))

/* photo letter 'A'..'V' -> mugshot TGA. Index = letter - 'A' (orig table @0x4962e0,
 * indexed (0x4961dc + letter*4); slot 0 = 'A'). All mugshots are 320x224. */
static const char *const k_credit_mugshots[] = {
    "Bob.tga", "Gareth.tga", "Snake.tga", "MikeT.tga", "Chris.tga", "Headley.tga", "Steve.tga",
    "Rich.tga", "Mike.tga", "Bez.tga", "Les.tga", "TonyP.tga", "JohnS.tga", "DavidT.tga", "TonyC.tga",
    "DaveyB.tga", "ChrisD.tga", "Slade.tga", "Matt.tga", "Marie.tga", "JFK.tga", "Daz.tga",
};
#define K_CREDIT_MUGSHOT_COUNT ((int)(sizeof(k_credit_mugshots)/sizeof(k_credit_mugshots[0])))

#endif /* TD5_CREDITS_H */

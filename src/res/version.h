// bms-dlss: verze vydani a udaje do VERSIONINFO (d3d9.rc, helper.rc) i do balicku (tools/release/pack.ps1).
// Jediny zdroj cisla verze vydani. BMSD_VERSION v bmsd_shared.h je verze rozhrani proxy <-> helper, ne vydani.
#pragma once

#define BMSDLSS_VER_MAJOR 1
#define BMSDLSS_VER_MINOR 1
#define BMSDLSS_VER_PATCH 0
#define BMSDLSS_VER_BUILD 0

// Retezcova podoba musi odpovidat cislum vyse (pack.ps1 cte BMSDLSS_VER_STR)
#define BMSDLSS_VER_STR "1.1.0"

#define BMSDLSS_PRODUCT   "bms-dlss"
#define BMSDLSS_COMPANY   "Crashdown"
#define BMSDLSS_COPYRIGHT "Copyright (c) 2026 Crashdown. MIT License."
#define BMSDLSS_COMMENTS  "Unofficial mod for Black Mesa. Not affiliated with or endorsed by Crowbar Collective, NVIDIA or AMD."

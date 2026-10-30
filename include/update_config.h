#pragma once

// Where the watch looks for firmware updates. These must match where
// .github/workflows/release.yml publishes: GitHub Pages for this repo, with
// manifest.json at the site root (see tools/make_release_site.py).
//
// To point a build at a different server (e.g. a local test server), override
// with -DOTA_SITE_URL=\"https://...\" in build_flags; the URL must be https.
#ifndef OTA_SITE_URL
#define OTA_SITE_URL "https://mitchbradley.github.io/PuttingWatch/"
#endif
#ifndef OTA_MANIFEST_URL
#define OTA_MANIFEST_URL OTA_SITE_URL "manifest.json"
#endif

// Where the Bye screen's QR codes point. The guide is the page the release
// workflow publishes next to the firmware (docs/guide/using.html); the
// watch is a Honu Golf product.
#ifndef HELP_URL
#define HELP_URL OTA_SITE_URL "guide/using.html"
#endif
#ifndef HONU_URL
#define HONU_URL "https://honugolf.com"
#endif

#include "updater.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

static void test_valid_repo(void) {
    CHECK(app_updater_valid_repo("mzgs/C-GUI-Template"));
    CHECK(app_updater_valid_repo("owner_1/repo.name-2"));
    CHECK(!app_updater_valid_repo(NULL));
    CHECK(!app_updater_valid_repo(""));
    CHECK(!app_updater_valid_repo("noslash"));
    CHECK(!app_updater_valid_repo("/repo"));
    CHECK(!app_updater_valid_repo("owner/"));
    CHECK(!app_updater_valid_repo("a/b/c"));
    CHECK(!app_updater_valid_repo("owner/../repo"));
    CHECK(!app_updater_valid_repo("owner/repo..name"));
    CHECK(!app_updater_valid_repo("owner/repo with space"));
}

static void test_extract_release_tag(void) {
    char tag[64];

    CHECK(app_updater_extract_release_tag(
        "https://github.com/mzgs/C-GUI-Template/releases/tag/v0.1.5", tag, sizeof tag));
    CHECK(strcmp(tag, "v0.1.5") == 0);

    CHECK(app_updater_extract_release_tag(
        "https://github.com/mzgs/C-GUI-Template/releases/tag/v1.2.3-beta.1?foo=bar", tag, sizeof tag));
    CHECK(strcmp(tag, "v1.2.3-beta.1") == 0);

    CHECK(app_updater_extract_release_tag(
        "https://github.com/mzgs/C-GUI-Template/releases/tag/0.2.0#notes", tag, sizeof tag));
    CHECK(strcmp(tag, "0.2.0") == 0);

    CHECK(!app_updater_extract_release_tag(
        "https://github.com/mzgs/C-GUI-Template/releases", tag, sizeof tag));
    CHECK(!app_updater_extract_release_tag(
        "https://github.com/mzgs/C-GUI-Template/releases/tag/", tag, sizeof tag));
    CHECK(!app_updater_extract_release_tag(
        "https://github.com/mzgs/C-GUI-Template/releases/tag/..", tag, sizeof tag));
    CHECK(!app_updater_extract_release_tag(
        "https://github.com/mzgs/C-GUI-Template/releases/tag/v1.0%20bad", tag, sizeof tag));

    char tiny[4];
    CHECK(!app_updater_extract_release_tag(
        "https://github.com/mzgs/C-GUI-Template/releases/tag/v0.1.5", tiny, sizeof tiny));
}

static void test_compare_versions(void) {
    CHECK(app_updater_compare_versions("v0.1.5", "0.1.4") == 1);
    CHECK(app_updater_compare_versions("0.1.5", "0.1.4") == 1);
    CHECK(app_updater_compare_versions("0.2.0", "0.1.9") == 1);
    CHECK(app_updater_compare_versions("1.0.0", "0.99.99") == 1);
    CHECK(app_updater_compare_versions("0.10.0", "0.2.0") == 1);
    CHECK(app_updater_compare_versions("V1.2.3.4", "1.2.3.3") == 1);

    CHECK(app_updater_compare_versions("v0.1.4", "0.1.4") == 0);
    CHECK(app_updater_compare_versions("1.2", "1.2.0") == 0);
    CHECK(app_updater_compare_versions("1.2.0+build5", "1.2.0") == 0);

    CHECK(app_updater_compare_versions("v0.1.3", "0.1.4") == -1);
    CHECK(app_updater_compare_versions("0.1.5-rc.1", "0.1.5") == -1);
    CHECK(app_updater_compare_versions("0.1.5", "0.1.5-rc.1") == 1);
    CHECK(app_updater_compare_versions("0.2.0-rc.2", "0.2.0-rc.1") == 1);
    CHECK(app_updater_compare_versions("0.2.0-rc.1", "0.2.0-rc.2") == -1);
    CHECK(app_updater_compare_versions("0.2.0-rc.10", "0.2.0-rc.2") == 1);
    CHECK(app_updater_compare_versions("0.2.0-beta", "0.2.0-alpha.1") == 1);
    CHECK(app_updater_compare_versions("0.2.0-alpha.1", "0.2.0-alpha") == 1);
    CHECK(app_updater_compare_versions("0.2.0-alpha", "0.2.0-1") == 1);
    CHECK(app_updater_compare_versions("0.2.0-rc.1+build.2", "0.2.0-rc.1+build.1") == 0);

    CHECK(app_updater_compare_versions("", "0.1.4") == -2);
    CHECK(app_updater_compare_versions("invalid", "0.1.4") == -2);
    CHECK(app_updater_compare_versions("1", "0.1.4") == -2);
    CHECK(app_updater_compare_versions("1.2.3.4.5", "0.1.4") == -2);
    CHECK(app_updater_compare_versions("1.2.", "0.1.4") == -2);
    CHECK(app_updater_compare_versions("1.2.3-", "0.1.4") == -2);
    CHECK(app_updater_compare_versions("1.2.3-rc..1", "0.1.4") == -2);
    CHECK(app_updater_compare_versions("1.2.3-bad space", "0.1.4") == -2);
    CHECK(app_updater_compare_versions("1.2.3+", "0.1.4") == -2);
    CHECK(app_updater_compare_versions("1.2.3+a..b", "0.1.4") == -2);
}

static void test_check_due(void) {
    const int64_t day = APP_UPDATE_CHECK_INTERVAL_SEC;
    const int64_t base = 1700000000;

    CHECK(app_updater_check_due(0, base));
    CHECK(app_updater_check_due(-1, base));
    CHECK(!app_updater_check_due(base, base));
    CHECK(!app_updater_check_due(base, base + day - 1));
    CHECK(app_updater_check_due(base, base + day));
    CHECK(app_updater_check_due(base, base + day + 3600));
    /* Clock moved backwards should still allow checking. */
    CHECK(app_updater_check_due(base + 100, base));
}

static void test_lifecycle(void) {
    app_updater *u = app_updater_create("mzgs/C-GUI-Template", "0.1.0");
    CHECK(u != NULL);
    CHECK(!app_updater_is_busy(u));
    app_update_snapshot snap;
    app_updater_snapshot(u, &snap);
    CHECK(snap.state == APP_UPDATE_IDLE);
    app_updater_destroy(u);

    CHECK(app_updater_create("invalid-repo", "0.1.0") == NULL);
    CHECK(app_updater_create("mzgs/C-GUI-Template", "") == NULL);
}

int main(void) {
    test_valid_repo();
    test_extract_release_tag();
    test_compare_versions();
    test_check_due();
    test_lifecycle();
    if (failures > 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("updater_tests: all passed\n");
    return 0;
}

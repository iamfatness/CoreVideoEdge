namespace CoreVideoPro.MediaCore.Services;

// Edge shim for the two MediaCorePaths members the lifted Zoom OAuth files
// (ZoomOAuthManifest, its tests) use. CoreVideo Pro's MediaCorePaths also
// locates its media core and repo layout, which Edge does not have.
public static class MediaCorePaths
{
    // Same variable as CoreVideo Pro: overrides the OAuth broker for staging.
    public const string ZoomOAuthBrokerStartUrlEnvVar = "COREVIDEO_ZOOM_OAUTH_BROKER_START_URL";

    // ZoomOAuthManifest.Load looks for <RepoRoot>/src/config/zoomOAuth.json and
    // falls back to the baked defaults (broker https://corevideo.iamfatness.us,
    // owner ruling 2026-10-25). Edge ships no such file, so the defaults apply.
    public static string RepoRoot => AppContext.BaseDirectory;
}

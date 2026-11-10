using System.Text.Json;
using System.Text.Json.Nodes;

namespace CoreVideoEdge;

// The node's edge.json (config/edge.example.json documents every key). The app
// edits only recordFolder and filePrefix and preserves everything else, so a
// hand-written config (subscribe list, cap, bitrate) survives the UI.
public static class EdgeSettings
{
    public static string DataDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CoreVideoEdge");

    public static string DefaultConfigPath => Path.Combine(DataDirectory, "edge.json");

    // configVersion 2 (2026-11-09): the v1 app wrote "maxSubscriptions": 6 as
    // its default, which pinned every upgraded node below the v2 cap of 8. A
    // v1 file still holding exactly that default is moved to 8 once; any other
    // value, or a 6 written after the migration, is the operator's and stays.
    public const int ConfigVersion = 2;

    public static void EnsureExists(string path)
    {
        if (!File.Exists(path))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, "{\n  \"configVersion\": 2,\n  \"filePrefix\": \"edge\",\n  \"maxSubscriptions\": 8,\n  \"subscribe\": []\n}\n");
            return;
        }
        JsonObject? root;
        try { root = JsonNode.Parse(File.ReadAllText(path))?.AsObject(); }
        catch (JsonException) { return; }  // never rewrite a file we could not read; the node reports it
        if (root is null) return;
        var version = root["configVersion"] is JsonValue v && v.TryGetValue<int>(out var n) ? n : 1;
        if (version >= ConfigVersion) return;
        if (root["maxSubscriptions"] is JsonValue cap && cap.TryGetValue<int>(out var c) && c == 6)
            root["maxSubscriptions"] = 8;
        root["configVersion"] = ConfigVersion;
        Write(path, root);
    }

    public static void Update(string path, string? recordFolder = null, string? filePrefix = null)
    {
        JsonObject root;
        try
        {
            root = File.Exists(path) ? JsonNode.Parse(File.ReadAllText(path))?.AsObject() ?? new JsonObject() : new JsonObject();
        }
        catch (JsonException)
        {
            // Never overwrite a config we could not read: the operator's hand
            // edits would be lost. Leave it for them to fix.
            throw new InvalidOperationException($"{path} is not valid JSON; fix or delete it.");
        }
        if (recordFolder is not null) root["recordFolder"] = recordFolder;
        if (filePrefix is not null) root["filePrefix"] = filePrefix;
        Write(path, root);
    }

    private static void Write(string path, JsonObject root)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + ".tmp";
        File.WriteAllText(temp, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        File.Move(temp, path, overwrite: true);
    }
}

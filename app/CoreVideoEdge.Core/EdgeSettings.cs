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

    public static void EnsureExists(string path)
    {
        if (File.Exists(path)) return;
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, "{\n  \"filePrefix\": \"edge\",\n  \"maxSubscriptions\": 6,\n  \"subscribe\": []\n}\n");
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
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + ".tmp";
        File.WriteAllText(temp, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        File.Move(temp, path, overwrite: true);
    }
}

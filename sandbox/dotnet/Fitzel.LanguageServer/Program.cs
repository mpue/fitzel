// The process: find Roslyn, then read requests from stdin, one JSON object per
// line, and answer each on stdout, one JSON object per line, tagged with the
// request's id. Requests run concurrently; a new request in the same "slot"
// (completion, analysis, hover...) cancels the one before it, so a fast typist
// never queues up stale work. Nothing but protocol goes to stdout -- logging
// goes to stderr, which the editor shows in its Log window.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace Fitzel.LanguageServer
{
    public static class Program
    {
        static readonly object writeLock = new object();
        static Stream stdout;

        public static int Main(string[] args)
        {
            CultureInfo.DefaultThreadCurrentCulture   = CultureInfo.InvariantCulture;
            CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.InvariantCulture;

            string roslyn = RoslynDirectory();
            if (roslyn == null)
            {
                Console.Error.WriteLine("[C# service] no Roslyn found: install the .NET 10 SDK");
                return 2;
            }
            // Before the first Roslyn type is touched: every assembly the
            // runtime does not have comes from the SDK's dotnet-format folder.
            AssemblyLoadContext.Default.Resolving += (ctx, name) =>
            {
                string p = Path.Combine(roslyn, name.Name + ".dll");
                return File.Exists(p) ? ctx.LoadFromAssemblyPath(p) : null;
            };
            Console.Error.WriteLine("[C# service] Roslyn from " + roslyn);
            stdout = Console.OpenStandardOutput();
            return Run();
        }

        // sdk/<version>/DotnetTools/dotnet-format of the installed .NET: the
        // SDK this was built against when it is still there, else the newest.
        static string RoslynDirectory()
        {
            string env = Environment.GetEnvironmentVariable("FITZEL_ROSLYN_DIR");
            if (!string.IsNullOrEmpty(env) && File.Exists(Path.Combine(env, "Microsoft.CodeAnalysis.Features.dll")))
                return env;
            string root = DotnetRoot();
            if (root == null) return null;
            string sdkRoot = Path.Combine(root, "sdk");
            if (!Directory.Exists(sdkRoot)) return null;
            string Tools(string sdk) => Path.Combine(sdk, "DotnetTools", "dotnet-format");
            bool Usable(string dir) => File.Exists(Path.Combine(dir, "Microsoft.CodeAnalysis.Features.dll"));

            string built = typeof(Program).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
                .FirstOrDefault(a => a.Key == "RoslynSdk")?.Value;
            if (!string.IsNullOrEmpty(built) && Usable(Tools(Path.Combine(sdkRoot, built))))
                return Tools(Path.Combine(sdkRoot, built));
            return Directory.GetDirectories(sdkRoot)
                .Where(d => Usable(Tools(d)))
                .OrderByDescending(d => ParseVersion(Path.GetFileName(d)))
                .Select(Tools)
                .FirstOrDefault();
        }

        // The runtime runs from <root>/shared/Microsoft.NETCore.App/<version>/.
        internal static string DotnetRoot()
        {
            string rt = Path.GetDirectoryName(typeof(object).Assembly.Location);
            string root = rt == null ? null : Path.GetFullPath(Path.Combine(rt, "..", "..", ".."));
            return root != null && Directory.Exists(root) ? root : null;
        }

        internal static Version ParseVersion(string s)
        {
            int dash = s.IndexOf('-');
            if (dash >= 0) s = s.Substring(0, dash);
            return Version.TryParse(s, out var v) ? v : new Version(0, 0);
        }

        // --- The loop -------------------------------------------------------
        [MethodImpl(MethodImplOptions.NoInlining)]
        static int Run()
        {
            var service = new Service();
            var slots = new Dictionary<string, CancellationTokenSource>();
            var input = new StreamReader(Console.OpenStandardInput(), new UTF8Encoding(false));
            Send(new JsonObject { ["event"] = "started" });
            string line;
            while ((line = input.ReadLine()) != null)
            {
                if (line.Length == 0) continue;
                JsonObject req;
                try { req = JsonNode.Parse(line) as JsonObject; }
                catch (Exception e) { Console.Error.WriteLine("[C# service] bad request: " + e.Message); continue; }
                if (req == null) continue;
                long id = req["id"]?.GetValue<long>() ?? 0;
                string op = req["op"]?.GetValue<string>() ?? "";
                if (op == "exit") break;

                // In order, on this thread: the project and the editor's text.
                // Only the work on that snapshot runs in the background.
                Func<CancellationToken, Task<JsonObject>> work;
                try { work = service.Prepare(op, req); }
                catch (Exception e) { Reply(id, op, null, e.Message); continue; }
                if (work == null) { Reply(id, op, new JsonObject(), null); continue; }

                string slot = req["slot"]?.GetValue<string>() ?? op;
                CancellationTokenSource cts = new CancellationTokenSource();
                lock (slots)
                {
                    if (slots.TryGetValue(slot, out var old)) old.Cancel();
                    slots[slot] = cts;
                }
                Task.Run(async () =>
                {
                    try
                    {
                        JsonObject result = await work(cts.Token).ConfigureAwait(false);
                        if (!cts.IsCancellationRequested) Reply(id, op, result ?? new JsonObject(), null);
                        else Reply(id, op, null, "cancelled");
                    }
                    catch (OperationCanceledException) { Reply(id, op, null, "cancelled"); }
                    catch (Exception e)
                    {
                        Console.Error.WriteLine("[C# service] " + op + ": " + e);
                        Reply(id, op, null, e.Message);
                    }
                });
            }
            return 0;
        }

        static void Reply(long id, string op, JsonObject result, string error)
        {
            var o = new JsonObject { ["id"] = id, ["op"] = op };
            if (error != null) o["error"] = error;
            else o["result"] = result;
            Send(o);
        }

        internal static void Send(JsonObject o)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(o.ToJsonString(new JsonSerializerOptions { WriteIndented = false }) + "\n");
            lock (writeLock)
            {
                stdout.Write(bytes, 0, bytes.Length);
                stdout.Flush();
            }
        }
    }
}

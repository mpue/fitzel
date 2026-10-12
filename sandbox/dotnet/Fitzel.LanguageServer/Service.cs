// The language service proper: one Roslyn workspace holding the open project's
// scripts exactly as Assembly-CSharp.csproj compiles them (every .cs under
// scripts/, the implicit and the csproj's own usings, the framework's reference
// assemblies and Library/Fitzel.dll with their XML docs), the editor's unsaved
// buffers laid over the files on disk.
//
// Positions on the wire are {line, ch}: 0-based line, ch counted in characters
// (code points) from the line's start, tabs as one -- what the editor's text
// buffer counts, so neither side needs the other's encoding.
using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Classification;
using Microsoft.CodeAnalysis.Completion;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FindSymbols;
using Microsoft.CodeAnalysis.Formatting;
using Microsoft.CodeAnalysis.Host.Mef;
using Microsoft.CodeAnalysis.QuickInfo;
using Microsoft.CodeAnalysis.Rename;
using Microsoft.CodeAnalysis.Tags;
using Microsoft.CodeAnalysis.Text;

namespace Fitzel.LanguageServer
{
    sealed class Service
    {
        static readonly StringComparer PathCmp =
            OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

        AdhocWorkspace workspace;
        ProjectId project;
        string projectDir, scriptsDir;
        readonly Dictionary<string, DocumentId> docs;          // full path -> document
        readonly Dictionary<string, DateTime> diskTimes;       // full path -> mtime read
        readonly HashSet<string> overlays;                     // paths the editor holds
        DateTime lastScan = DateTime.MinValue;
        DateTime apiTime = DateTime.MinValue;                  // Library/Fitzel.dll as referenced

        // The last completion list, for describe/commit of one of its items.
        Document completionDoc;
        CompletionList completionList;
        int completionPos;

        public Service()
        {
            docs = new Dictionary<string, DocumentId>(PathCmp);
            diskTimes = new Dictionary<string, DateTime>(PathCmp);
            overlays = new HashSet<string>(PathCmp);
        }

        // Runs on the reader thread, in request order: everything that changes
        // the workspace. Returns the work to do on the resulting snapshot (null:
        // nothing more to do).
        public Func<CancellationToken, Task<JsonObject>> Prepare(string op, JsonObject req)
        {
            if (op == "project")
            {
                OpenProject(Str(req, "dir"));
                return null;
            }
            if (workspace == null) throw new InvalidOperationException("no project");
            if ((DateTime.UtcNow - lastScan).TotalSeconds > 1.0) Scan();

            string file = Str(req, "file");
            string path = file == null ? null : Full(file);
            if (op == "close")
            {
                if (path != null && overlays.Remove(path))
                {
                    diskTimes.Remove(path);
                    Scan();
                }
                return null;
            }
            if (path != null && req["text"] != null) SetOverlay(path, Str(req, "text"));
            if (op == "text") return null;

            Solution solution = workspace.CurrentSolution;
            Document doc = null;
            if (path != null && docs.TryGetValue(path, out var did)) doc = solution.GetDocument(did);
            if (doc == null && op != "warm" && op != "describe") throw new InvalidOperationException("not in the project: " + file);
            int line = Int(req, "line"), ch = Int(req, "ch");

            switch (op)
            {
                case "complete":  return ct => Complete(doc, line, ch, Str(req, "trigger"), ct);
                case "describe":  return ct => Describe(Int(req, "index"), ct);
                case "commit":    return ct => Commit(doc, line, ch, Int(req, "index"), ct);
                case "hover":     return ct => Hover(doc, line, ch, ct);
                case "signature": return ct => Signature(doc, line, ch, ct);
                case "analyze":   return ct => Analyze(doc, ct);
                case "definition":return ct => Definition(doc, line, ch, ct);
                case "references":return ct => References(doc, line, ch, ct);
                case "rename":    return ct => Rename(doc, line, ch, Str(req, "name"), ct);
                case "format":    return ct => Format(doc, ct);
                case "warm":      return ct => Warm(ct);
                default: throw new InvalidOperationException("unknown op " + op);
            }
        }

        // --- The project ----------------------------------------------------
        void OpenProject(string dir)
        {
            projectDir = Path.GetFullPath(dir);
            scriptsDir = Path.Combine(projectDir, "scripts");
            docs.Clear(); diskTimes.Clear(); overlays.Clear();
            completionDoc = null; completionList = null;

            string csproj = Directory.Exists(projectDir)
                ? Directory.GetFiles(projectDir, "*.csproj").FirstOrDefault() : null;
            string props = csproj != null ? File.ReadAllText(csproj) : "";
            bool Flag(string name, string value) =>
                Regex.IsMatch(props, "<" + name + ">\\s*" + value + "\\s*</" + name + ">", RegexOptions.IgnoreCase);

            var host = MefHostServices.Create(MefHostServices.DefaultAssemblies);
            workspace = new AdhocWorkspace(host);
            project = ProjectId.CreateNewId();
            var parse = new CSharpParseOptions(LanguageVersion.Preview, DocumentationMode.Parse,
                                               SourceCodeKind.Regular, new[] { "DEBUG", "TRACE", "NET", "NET10_0", "NETCOREAPP", "FITZEL" });
            var compile = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary,
                allowUnsafe: Flag("AllowUnsafeBlocks", "true"),
                nullableContextOptions: Flag("Nullable", "enable") ? NullableContextOptions.Enable : NullableContextOptions.Disable);
            var info = ProjectInfo.Create(project, VersionStamp.Create(), "Assembly-CSharp", "Assembly-CSharp",
                                          LanguageNames.CSharp, compilationOptions: compile, parseOptions: parse,
                                          metadataReferences: References());
            Solution s = workspace.CurrentSolution.AddProject(info);

            // The usings every script sees without writing them.
            var usings = new List<string>();
            if (csproj == null || Flag("ImplicitUsings", "(enable|true)"))
                usings.AddRange(new[] { "System", "System.Collections.Generic", "System.IO", "System.Linq",
                                        "System.Net.Http", "System.Threading", "System.Threading.Tasks" });
            foreach (Match m in Regex.Matches(props, "<Using\\s+Include=\"([^\"]+)\"\\s*/>"))
                usings.Add(m.Groups[1].Value);
            if (csproj == null) usings.AddRange(new[] { "Fitzel", "System.Numerics" });
            string global = string.Concat(usings.Distinct().Select(u => "global using global::" + u + ";\n"));
            s = s.AddDocument(DocumentId.CreateNewId(project), "GlobalUsings.g.cs", SourceText.From(global),
                              filePath: Path.Combine(projectDir, "Library", "obj", "GlobalUsings.g.cs"));
            // The editor's buffers end lines with \n; so does what the formatter writes.
            s = s.AddAnalyzerConfigDocument(DocumentId.CreateNewId(project), ".editorconfig",
                SourceText.From("root = true\n[*.cs]\nend_of_line = lf\nindent_style = space\nindent_size = 4\n"),
                filePath: Path.Combine(projectDir, ".editorconfig"));   // covers scripts/
            if (!workspace.TryApplyChanges(s)) throw new InvalidOperationException("workspace refused the project");
            Scan();
            Console.Error.WriteLine("[C# service] project " + projectDir + ": " + docs.Count + " scripts");
        }

        // The framework's reference assemblies (the newest Microsoft.NETCore.App.Ref
        // pack, docs beside them) and the engine's API.
        List<MetadataReference> References()
        {
            var refs = new List<MetadataReference>();
            string root = Program.DotnetRoot();
            string pack = root == null ? null : Path.Combine(root, "packs", "Microsoft.NETCore.App.Ref");
            string refDir = null;
            if (pack != null && Directory.Exists(pack))
                refDir = Directory.GetDirectories(pack)
                    .Where(d => Path.GetFileName(d).StartsWith("10."))
                    .OrderByDescending(d => Program.ParseVersion(Path.GetFileName(d)))
                    .Select(d => Path.Combine(d, "ref", "net10.0"))
                    .FirstOrDefault(Directory.Exists);
            // No targeting pack: compile against the runtime itself (no docs).
            refDir ??= Path.GetDirectoryName(typeof(object).Assembly.Location);
            foreach (string dll in Directory.GetFiles(refDir, "*.dll"))
            {
                if (!IsManaged(dll)) continue;
                string xml = Path.ChangeExtension(dll, ".xml");
                refs.Add(MetadataReference.CreateFromFile(dll,
                    documentation: File.Exists(xml) ? XmlDocumentationProvider.CreateFromFile(xml) : null));
            }
            string api = Path.Combine(projectDir, "Library", "Fitzel.dll");
            if (File.Exists(api))
            {
                apiTime = File.GetLastWriteTimeUtc(api);
                string xml = Path.ChangeExtension(api, ".xml");
                // As bytes: the editor replaces this file when the engine's is newer.
                refs.Add(MetadataReference.CreateFromImage(File.ReadAllBytes(api),
                    documentation: File.Exists(xml) ? XmlDocumentationProvider.CreateFromBytes(File.ReadAllBytes(xml)) : null,
                    filePath: api));
            }
            return refs;
        }

        static bool IsManaged(string dll)
        {
            try
            {
                using var fs = File.OpenRead(dll);
                using var pe = new System.Reflection.PortableExecutable.PEReader(fs);
                return pe.HasMetadata;
            }
            catch { return false; }
        }

        // The scripts on disk: new files in, deleted ones out, changed ones
        // re-read -- except what the editor holds (its text wins).
        void Scan()
        {
            lastScan = DateTime.UtcNow;
            Solution s = workspace.CurrentSolution;
            string api = Path.Combine(projectDir, "Library", "Fitzel.dll");
            if (File.Exists(api) && File.GetLastWriteTimeUtc(api) != apiTime)
                s = s.WithProjectMetadataReferences(project, References());

            var seen = new HashSet<string>(PathCmp);
            if (Directory.Exists(scriptsDir))
                foreach (string f in Directory.EnumerateFiles(scriptsDir, "*.cs", SearchOption.AllDirectories))
                {
                    string p = Path.GetFullPath(f);
                    seen.Add(p);
                    if (overlays.Contains(p)) continue;
                    DateTime t = File.GetLastWriteTimeUtc(p);
                    if (diskTimes.TryGetValue(p, out var known) && known == t && docs.ContainsKey(p)) continue;
                    string text;
                    try { text = File.ReadAllText(p); } catch (IOException) { continue; }
                    diskTimes[p] = t;
                    s = Put(s, p, text);
                }
            foreach (string p in docs.Keys.ToList())
                if (!seen.Contains(p) && !overlays.Contains(p))
                {
                    s = s.RemoveDocument(docs[p]);
                    docs.Remove(p); diskTimes.Remove(p);
                }
            if (s != workspace.CurrentSolution) workspace.TryApplyChanges(s);
        }

        void SetOverlay(string path, string text)
        {
            overlays.Add(path);
            Solution s = Put(workspace.CurrentSolution, path, text);
            if (s != workspace.CurrentSolution) workspace.TryApplyChanges(s);
        }

        Solution Put(Solution s, string path, string text)
        {
            if (docs.TryGetValue(path, out var id))
            {
                Document d = s.GetDocument(id);
                if (d != null && d.TryGetText(out var old) && old.ToString() == text) return s;
                return s.WithDocumentText(id, SourceText.From(text, Encoding.UTF8));
            }
            id = DocumentId.CreateNewId(project);
            docs[path] = id;
            string rel = Path.GetRelativePath(projectDir, path);
            var folders = Path.GetDirectoryName(rel)?.Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries);
            return s.AddDocument(id, Path.GetFileName(path), SourceText.From(text, Encoding.UTF8), folders, path);
        }

        string Full(string file) => Path.GetFullPath(Path.IsPathRooted(file) ? file : Path.Combine(projectDir, file));

        // --- Features ---------------------------------------------------------
        async Task<JsonObject> Warm(CancellationToken ct)
        {
            Document d = workspace.CurrentSolution.GetDocument(docs.Values.FirstOrDefault())
                ?? workspace.CurrentSolution.Projects.First().Documents.First();
            await d.GetSemanticModelAsync(ct).ConfigureAwait(false);
            var text = await d.GetTextAsync(ct).ConfigureAwait(false);
            CompletionService svc = CompletionService.GetService(d);
            if (svc != null) await svc.GetCompletionsAsync(d, text.Length, cancellationToken: ct).ConfigureAwait(false);
            return new JsonObject();
        }

        async Task<JsonObject> Complete(Document doc, int line, int ch, string trigger, CancellationToken ct)
        {
            SourceText text = await doc.GetTextAsync(ct).ConfigureAwait(false);
            int pos = Offset(text, line, ch);
            var result = new JsonObject();
            // Inside a string that names something of the editor's (a scene
            // object for Entity.Find): the editor fills the list, it knows the names.
            JsonObject inString = await StringContext(doc, text, pos, ct).ConfigureAwait(false);
            if (inString != null) return inString;
            CompletionService svc = CompletionService.GetService(doc);
            if (svc == null) return result;
            CompletionTrigger how = string.IsNullOrEmpty(trigger)
                ? CompletionTrigger.Invoke : CompletionTrigger.CreateInsertionTrigger(trigger[0]);
            if (how.Kind == CompletionTriggerKind.Insertion && !svc.ShouldTriggerCompletion(text, pos, how))
                return result;
            CompletionList list = await svc.GetCompletionsAsync(doc, pos, how, cancellationToken: ct).ConfigureAwait(false);
            if (list == null || list.ItemsList.Count == 0) return result;

            var items = new JsonArray();
            foreach (CompletionItem it in list.ItemsList)
            {
                var o = new JsonObject
                {
                    ["l"] = it.DisplayTextPrefix + it.DisplayText + it.DisplayTextSuffix,
                    ["k"] = KindOf(it.Tags),
                };
                if (it.FilterText != it.DisplayText) o["f"] = it.FilterText;
                if (it.SortText != it.DisplayText) o["s"] = it.SortText;
                if (!string.IsNullOrEmpty(it.InlineDescription)) o["d"] = it.InlineDescription;
                if (it.Rules.MatchPriority != 0) o["p"] = it.Rules.MatchPriority;
                items.Add(o);
            }
            lock (this) { completionDoc = doc; completionList = list; completionPos = pos; }
            result["items"] = items;
            result["start"] = Pos(text, list.Span.Start);
            result["suggestion"] = list.SuggestionModeItem != null;
            return result;
        }

        // Fitzel API parameters whose strings are names the editor knows:
        // (type, method, argument index) -> what kind of name.
        static readonly (string type, string method, int arg, string kind)[] NamedStrings =
        {
            ("Fitzel.Entity", "Find", 0, "entity"),
            ("Fitzel.Entity", "FindAll", 0, "entity"),
        };

        // The caret inside a string literal passed to one of those: {"string":
        // kind, "start": where the string's text begins}. Null otherwise.
        static async Task<JsonObject> StringContext(Document doc, SourceText text, int pos, CancellationToken ct)
        {
            SyntaxNode root = await doc.GetSyntaxRootAsync(ct).ConfigureAwait(false);
            SyntaxToken token = root.FindToken(Math.Max(0, pos - 1));
            if (!token.IsKind(SyntaxKind.StringLiteralToken)) return null;
            int quote = token.Text.IndexOf('"');
            if (quote < 0) return null;
            int contentStart = token.SpanStart + quote + 1;
            bool closed = token.Text.Length > quote + 1 && token.Text.EndsWith("\"");
            if (pos < contentStart || (closed && pos >= token.Span.End)) return null;
            if (token.Parent is not LiteralExpressionSyntax lit || lit.Parent is not ArgumentSyntax arg ||
                arg.Parent is not ArgumentListSyntax args || args.Parent is not InvocationExpressionSyntax call)
                return null;
            int index = args.Arguments.IndexOf(arg);
            SemanticModel model = await doc.GetSemanticModelAsync(ct).ConfigureAwait(false);
            SymbolInfo info = model.GetSymbolInfo(call, ct);
            if ((info.Symbol ?? info.CandidateSymbols.FirstOrDefault()) is not IMethodSymbol m) return null;
            string type = m.ContainingType?.ToDisplayString();
            foreach (var n in NamedStrings)
                if (n.type == type && n.method == m.Name && n.arg == index)
                    return new JsonObject { ["string"] = n.kind, ["start"] = Pos(text, contentStart) };
            return null;
        }

        async Task<JsonObject> Describe(int index, CancellationToken ct)
        {
            Document doc; CompletionList list;
            lock (this) { doc = completionDoc; list = completionList; }
            if (list == null || index < 0 || index >= list.ItemsList.Count) return new JsonObject();
            CompletionService svc = CompletionService.GetService(doc);
            CompletionDescription d = await svc.GetDescriptionAsync(doc, list.ItemsList[index], ct).ConfigureAwait(false);
            return new JsonObject { ["text"] = Lf(d?.Text ?? "") };
        }

        // Accept item `index` of the last list. The list was made for an older
        // text: the user has typed on since (the filter). The change is made
        // against that older text -- its result is the same text either way --
        // and handed back as edits to the CURRENT text plus where the caret goes.
        async Task<JsonObject> Commit(Document current, int line, int ch, int index, CancellationToken ct)
        {
            Document doc; CompletionList list; int origPos;
            lock (this) { doc = completionDoc; list = completionList; origPos = completionPos; }
            if (list == null || index < 0 || index >= list.ItemsList.Count) return new JsonObject();
            CompletionService svc = CompletionService.GetService(doc);
            CompletionChange change = await svc.GetChangeAsync(doc, list.ItemsList[index], null, ct).ConfigureAwait(false);
            SourceText orig = await doc.GetTextAsync(ct).ConfigureAwait(false);
            SourceText cur = await current.GetTextAsync(ct).ConfigureAwait(false);
            int curPos = Offset(cur, line, ch);
            int delta = curPos - origPos;

            var changes = change.TextChanges.IsDefaultOrEmpty
                ? new List<TextChange> { change.TextChange } : change.TextChanges.OrderBy(c => c.Span.Start).ToList();
            SourceText after = orig.WithChanges(changes);
            // Where the primary change's text ends in `after`: the caret when the
            // provider names none.
            int shift = 0, caret = -1;
            foreach (TextChange c in changes)
            {
                if (c.Span == change.TextChange.Span && c.NewText == change.TextChange.NewText)
                    caret = c.Span.Start + shift + c.NewText.Length;
                shift += c.NewText.Length - c.Span.Length;
            }
            if (change.NewPosition.HasValue) caret = change.NewPosition.Value;
            if (caret < 0) caret = after.Length;

            var edits = new JsonArray();
            foreach (TextChange c in changes)
            {
                int s = c.Span.Start, e = c.Span.End;
                if (s > origPos) s += delta;
                if (e >= origPos) e += Math.Max(delta, 0);
                s = Math.Clamp(s, 0, cur.Length); e = Math.Clamp(e, s, cur.Length);
                edits.Add(Edit(cur, s, e, c.NewText));
            }
            return new JsonObject { ["edits"] = edits, ["caret"] = Pos(after, Math.Clamp(caret, 0, after.Length)) };
        }

        async Task<JsonObject> Hover(Document doc, int line, int ch, CancellationToken ct)
        {
            SourceText text = await doc.GetTextAsync(ct).ConfigureAwait(false);
            int pos = Offset(text, line, ch);
            QuickInfoService svc = QuickInfoService.GetService(doc);
            if (svc == null) return new JsonObject();
            QuickInfoItem info = await svc.GetQuickInfoAsync(doc, pos, ct).ConfigureAwait(false);
            if (info == null || info.Sections.IsDefaultOrEmpty) return new JsonObject();
            var sections = new JsonArray();
            foreach (QuickInfoSection s in info.Sections)
            {
                if (string.IsNullOrWhiteSpace(s.Text)) continue;
                sections.Add(new JsonObject { ["kind"] = s.Kind, ["text"] = Lf(s.Text) });
            }
            return new JsonObject
            {
                ["sections"] = sections,
                ["start"] = Pos(text, info.Span.Start),
                ["end"] = Pos(text, info.Span.End),
            };
        }

        // Signature help, by hand (Roslyn keeps its provider internal): the
        // innermost argument list around the caret, the methods the call could
        // mean, which argument the caret is in.
        async Task<JsonObject> Signature(Document doc, int line, int ch, CancellationToken ct)
        {
            SourceText text = await doc.GetTextAsync(ct).ConfigureAwait(false);
            int pos = Offset(text, line, ch);
            SyntaxNode root = await doc.GetSyntaxRootAsync(ct).ConfigureAwait(false);
            SemanticModel model = await doc.GetSemanticModelAsync(ct).ConfigureAwait(false);
            SyntaxToken token = root.FindToken(Math.Max(0, pos - 1));

            for (SyntaxNode n = token.Parent; n != null; n = n.Parent)
            {
                if (n is StatementSyntax || n is MemberDeclarationSyntax) break;
                SyntaxToken open, close;
                SeparatedSyntaxList<SyntaxNode> args;
                if (n is ArgumentListSyntax al) { open = al.OpenParenToken; close = al.CloseParenToken; args = SeparatedOf(al.Arguments); }
                else if (n is BracketedArgumentListSyntax bl) { open = bl.OpenBracketToken; close = bl.CloseBracketToken; args = SeparatedOf(bl.Arguments); }
                else if (n is AttributeArgumentListSyntax aal) { open = aal.OpenParenToken; close = aal.CloseParenToken; args = SeparatedOf(aal.Arguments); }
                else continue;
                if (pos <= open.SpanStart || (!close.IsMissing && pos > close.SpanStart)) continue;

                int active = args.GetSeparators().Count(s => s.SpanStart < pos);
                ISymbol chosen = null;
                var candidates = new List<ISymbol>();
                switch (n.Parent)
                {
                    case InvocationExpressionSyntax inv:
                        chosen = model.GetSymbolInfo(inv, ct).Symbol;
                        candidates.AddRange(model.GetMemberGroup(inv.Expression, ct));
                        break;
                    case BaseObjectCreationExpressionSyntax oc:
                    {
                        chosen = model.GetSymbolInfo(oc, ct).Symbol;
                        if (model.GetTypeInfo(oc, ct).Type is INamedTypeSymbol t) candidates.AddRange(t.InstanceConstructors);
                        break;
                    }
                    case ConstructorInitializerSyntax ci:
                    {
                        var si = model.GetSymbolInfo(ci, ct);
                        chosen = si.Symbol;
                        INamedTypeSymbol t = (chosen ?? si.CandidateSymbols.FirstOrDefault())?.ContainingType;
                        if (t != null) candidates.AddRange(t.InstanceConstructors);
                        break;
                    }
                    case ElementAccessExpressionSyntax ea:
                        chosen = model.GetSymbolInfo(ea, ct).Symbol;
                        candidates.AddRange(model.GetIndexerGroup(ea.Expression, ct));
                        break;
                    case AttributeSyntax at:
                    {
                        chosen = model.GetSymbolInfo(at, ct).Symbol;
                        if (model.GetTypeInfo(at, ct).Type is INamedTypeSymbol t) candidates.AddRange(t.InstanceConstructors);
                        break;
                    }
                    default: continue;
                }
                candidates = candidates.Where(c => model.IsAccessible(pos, c))
                    .Distinct(SymbolEqualityComparer.Default).ToList();
                if (candidates.Count == 0) return new JsonObject();

                var sigs = new JsonArray();
                int activeSig = -1;
                for (int i = 0; i < candidates.Count; ++i)
                {
                    ISymbol c = candidates[i];
                    ImmutableArray<IParameterSymbol> ps = c is IMethodSymbol m ? m.Parameters
                        : c is IPropertySymbol p ? p.Parameters : ImmutableArray<IParameterSymbol>.Empty;
                    sigs.Add(Describe(c, ps, model, pos));
                    if (chosen != null && SymbolEqualityComparer.Default.Equals(chosen.OriginalDefinition, c.OriginalDefinition))
                        activeSig = i;
                }
                if (activeSig < 0)
                {
                    // The first that has room for the argument the caret is in.
                    activeSig = 0;
                    for (int i = 0; i < candidates.Count; ++i)
                    {
                        var ps = candidates[i] is IMethodSymbol m ? m.Parameters
                            : candidates[i] is IPropertySymbol p ? p.Parameters : ImmutableArray<IParameterSymbol>.Empty;
                        if (ps.Length > active || ps.Any(q => q.IsParams)) { activeSig = i; break; }
                    }
                }
                return new JsonObject { ["sigs"] = sigs, ["active"] = activeSig, ["param"] = active };
            }
            return new JsonObject();
        }

        static SeparatedSyntaxList<SyntaxNode> SeparatedOf<T>(SeparatedSyntaxList<T> list) where T : SyntaxNode
            => SyntaxFactory.SeparatedList<SyntaxNode>(list.GetWithSeparators());

        static JsonObject Describe(ISymbol c, ImmutableArray<IParameterSymbol> ps, SemanticModel model, int pos)
        {
            var label = new StringBuilder();
            var spans = new JsonArray();
            string Type(ITypeSymbol t) => t.ToMinimalDisplayString(model, pos);
            bool indexer = c is IPropertySymbol;
            if (c is IMethodSymbol m)
            {
                if (m.MethodKind == MethodKind.Constructor) label.Append(m.ContainingType.Name);
                else
                {
                    label.Append(Type(m.ReturnType)).Append(' ').Append(m.Name);
                    if (m.TypeParameters.Length > 0)
                        label.Append('<').Append(string.Join(", ", m.TypeParameters.Select(t => t.Name))).Append('>');
                }
            }
            else if (c is IPropertySymbol p) label.Append(Type(p.Type)).Append(" this");
            label.Append(indexer ? '[' : '(');
            string xml = c.GetDocumentationCommentXml(expandIncludes: true);
            XElement docs = null;
            try { if (!string.IsNullOrWhiteSpace(xml)) docs = XElement.Parse(xml); } catch { }
            for (int i = 0; i < ps.Length; ++i)
            {
                if (i > 0) label.Append(", ");
                int start = label.Length;
                IParameterSymbol q = ps[i];
                if (q.IsParams) label.Append("params ");
                if (q.RefKind == RefKind.Ref) label.Append("ref ");
                else if (q.RefKind == RefKind.Out) label.Append("out ");
                else if (q.RefKind == RefKind.In) label.Append("in ");
                label.Append(Type(q.Type)).Append(' ').Append(q.Name);
                if (q.HasExplicitDefaultValue)
                    label.Append(" = ").Append(q.ExplicitDefaultValue == null ? "null"
                        : q.ExplicitDefaultValue is string str ? "\"" + str + "\""
                        : q.ExplicitDefaultValue is bool b ? (b ? "true" : "false")
                        : Convert.ToString(q.ExplicitDefaultValue, System.Globalization.CultureInfo.InvariantCulture));
                var pd = docs?.Elements("param").FirstOrDefault(e => (string)e.Attribute("name") == q.Name);
                spans.Add(new JsonObject { ["s"] = start, ["e"] = label.Length, ["doc"] = DocText(pd) });
            }
            label.Append(indexer ? ']' : ')');
            return new JsonObject { ["label"] = label.ToString(), ["params"] = spans, ["doc"] = DocText(docs?.Element("summary")) };
        }

        // An XML doc element as plain text: <see cref="T:A.B"/> -> B,
        // <paramref name="x"/> -> x, whitespace collapsed.
        static string DocText(XElement e)
        {
            if (e == null) return "";
            var sb = new StringBuilder();
            foreach (XNode node in e.Nodes())
            {
                if (node is XText t) sb.Append(t.Value);
                else if (node is XElement x)
                {
                    string cref = (string)x.Attribute("cref") ?? (string)x.Attribute("langword") ?? (string)x.Attribute("name");
                    if (x.Name == "para") sb.Append('\n').Append(DocText(x)).Append('\n');
                    else if (cref != null && x.IsEmpty)
                    {
                        int colon = cref.IndexOf(':');
                        string name = colon >= 0 ? cref.Substring(colon + 1) : cref;
                        int paren = name.IndexOf('(');
                        if (paren >= 0) name = name.Substring(0, paren);
                        int dot = name.LastIndexOf('.');
                        sb.Append(dot >= 0 ? name.Substring(dot + 1) : name);
                    }
                    else sb.Append(DocText(x));
                }
            }
            return Regex.Replace(sb.ToString(), "[ \\t\\r]*\\n[ \\t\\r]*\\n[\\s]*", "\n\n")
                        .Replace("\r", "").Trim()
                        .Split('\n').Select(l => Regex.Replace(l, "\\s+", " ").Trim())
                        .Aggregate(new StringBuilder(), (b, l) => b.Append(b.Length > 0 ? "\n" : "").Append(l))
                        .ToString();
        }

        // Errors and warnings of the file, and the semantic colours the
        // editor's own lexer cannot know (types, methods, locals...).
        async Task<JsonObject> Analyze(Document doc, CancellationToken ct)
        {
            SourceText text = await doc.GetTextAsync(ct).ConfigureAwait(false);
            SemanticModel model = await doc.GetSemanticModelAsync(ct).ConfigureAwait(false);
            var diags = new JsonArray();
            foreach (Diagnostic d in model.GetDiagnostics(null, ct))
            {
                if (d.Severity == DiagnosticSeverity.Hidden || !d.Location.IsInSource) continue;
                TextSpan span = d.Location.SourceSpan;
                diags.Add(new JsonObject
                {
                    ["start"] = Pos(text, span.Start),
                    ["end"] = Pos(text, span.End),
                    ["sev"] = d.Severity == DiagnosticSeverity.Error ? 3 : d.Severity == DiagnosticSeverity.Warning ? 2 : 1,
                    ["code"] = d.Id,
                    ["msg"] = d.GetMessage(),
                });
            }
            var tokens = new JsonArray();
            var spans = await Classifier.GetClassifiedSpansAsync(doc, new TextSpan(0, text.Length), ct).ConfigureAwait(false);
            foreach (ClassifiedSpan cs in spans)
            {
                int cat = Category(cs.ClassificationType);
                if (cat == 0 || cs.TextSpan.IsEmpty) continue;
                JsonObject a = Pos(text, cs.TextSpan.Start), b = Pos(text, cs.TextSpan.End);
                int l0 = (int)a["line"], c0 = (int)a["ch"], l1 = (int)b["line"], c1 = (int)b["ch"];
                if (l0 != l1)   // excluded code across lines: one entry per line
                {
                    for (int l = l0; l <= l1; ++l)
                    {
                        int s = l == l0 ? c0 : 0;
                        int e = l == l1 ? c1 : CharCount(text, text.Lines[l]);
                        if (e > s) { tokens.Add(l); tokens.Add(s); tokens.Add(e - s); tokens.Add(cat); }
                    }
                    continue;
                }
                tokens.Add(l0); tokens.Add(c0); tokens.Add(c1 - c0); tokens.Add(cat);
            }
            return new JsonObject { ["diags"] = diags, ["tokens"] = tokens };
        }

        static int Category(string c)
        {
            switch (c)
            {
                case ClassificationTypeNames.ClassName:
                case ClassificationTypeNames.RecordClassName:
                case ClassificationTypeNames.StructName:
                case ClassificationTypeNames.RecordStructName:
                case ClassificationTypeNames.InterfaceName:
                case ClassificationTypeNames.EnumName:
                case ClassificationTypeNames.DelegateName:
                case ClassificationTypeNames.ModuleName:
                case ClassificationTypeNames.TypeParameterName:  return 1;
                case ClassificationTypeNames.MethodName:
                case ClassificationTypeNames.ExtensionMethodName: return 2;
                case ClassificationTypeNames.PropertyName:
                case ClassificationTypeNames.FieldName:
                case ClassificationTypeNames.EventName:          return 3;
                case ClassificationTypeNames.LocalName:
                case ClassificationTypeNames.ParameterName:      return 4;
                case ClassificationTypeNames.EnumMemberName:
                case ClassificationTypeNames.ConstantName:       return 5;
                case ClassificationTypeNames.NamespaceName:      return 6;
                case ClassificationTypeNames.ControlKeyword:     return 7;
                case ClassificationTypeNames.Keyword:            return 8;
                case ClassificationTypeNames.ExcludedCode:       return 9;
                case ClassificationTypeNames.StringEscapeCharacter: return 10;
                default: return 0;
            }
        }

        async Task<ISymbol> SymbolAt(Document doc, int line, int ch, CancellationToken ct)
        {
            SourceText text = await doc.GetTextAsync(ct).ConfigureAwait(false);
            int pos = Offset(text, line, ch);
            ISymbol s = await SymbolFinder.FindSymbolAtPositionAsync(doc, pos, ct).ConfigureAwait(false);
            // The caret just after a name (end of a word) means that name.
            if (s == null && pos > 0)
                s = await SymbolFinder.FindSymbolAtPositionAsync(doc, pos - 1, ct).ConfigureAwait(false);
            if (s is IMethodSymbol m && m.ReducedFrom != null) s = m.ReducedFrom;
            return s;
        }

        async Task<JsonObject> Definition(Document doc, int line, int ch, CancellationToken ct)
        {
            ISymbol s = await SymbolAt(doc, line, ch, ct).ConfigureAwait(false);
            if (s == null) return new JsonObject();
            var locs = new JsonArray();
            foreach (Location loc in s.Locations.Where(l => l.IsInSource))
            {
                SourceText t = await loc.SourceTree.GetTextAsync(ct).ConfigureAwait(false);
                JsonObject p = Pos(t, loc.SourceSpan.Start);
                p["file"] = loc.SourceTree.FilePath;
                locs.Add(p);
            }
            var result = new JsonObject { ["locations"] = locs, ["name"] = s.ToDisplayString() };
            if (locs.Count == 0)
            {
                // A symbol from Fitzel.dll or the framework: what it is.
                result["metadata"] = s.ToDisplayString(SymbolDisplayFormat.CSharpErrorMessageFormat);
                result["assembly"] = s.ContainingAssembly?.Name ?? "";
                string xml = s.GetDocumentationCommentXml(expandIncludes: true);
                try { if (!string.IsNullOrWhiteSpace(xml)) result["doc"] = DocText(XElement.Parse(xml).Element("summary")); }
                catch { }
            }
            return result;
        }

        async Task<JsonObject> References(Document doc, int line, int ch, CancellationToken ct)
        {
            ISymbol s = await SymbolAt(doc, line, ch, ct).ConfigureAwait(false);
            if (s == null) return new JsonObject();
            var found = await SymbolFinder.FindReferencesAsync(s, doc.Project.Solution, ct).ConfigureAwait(false);
            var refs = new JsonArray();
            var seen = new HashSet<(string, int)>();
            async Task Add(Location loc, bool definition)
            {
                if (!loc.IsInSource || !seen.Add((loc.SourceTree.FilePath, loc.SourceSpan.Start))) return;
                SourceText t = await loc.SourceTree.GetTextAsync(ct).ConfigureAwait(false);
                JsonObject p = Pos(t, loc.SourceSpan.Start);
                p["file"] = loc.SourceTree.FilePath;
                p["len"] = loc.SourceSpan.Length;
                p["text"] = t.Lines.GetLineFromPosition(loc.SourceSpan.Start).ToString().Trim();
                p["def"] = definition;
                refs.Add(p);
            }
            foreach (ReferencedSymbol r in found)
            {
                foreach (Location l in r.Definition.Locations) await Add(l, true).ConfigureAwait(false);
                foreach (ReferenceLocation l in r.Locations) await Add(l.Location, false).ConfigureAwait(false);
            }
            return new JsonObject { ["refs"] = refs, ["name"] = s.Name };
        }

        async Task<JsonObject> Rename(Document doc, int line, int ch, string name, CancellationToken ct)
        {
            ISymbol s = await SymbolAt(doc, line, ch, ct).ConfigureAwait(false);
            if (s == null) return new JsonObject { ["error"] = "nothing to rename here" };
            if (!s.Locations.Any(l => l.IsInSource))
                return new JsonObject { ["error"] = s.Name + " is not the project's own (" + (s.ContainingAssembly?.Name ?? "?") + ")" };
            if (string.IsNullOrEmpty(name) || !SyntaxFacts.IsValidIdentifier(name))
                return new JsonObject { ["error"] = "'" + name + "' is not a valid name" };
            Solution before = doc.Project.Solution;
            Solution after = await Renamer.RenameSymbolAsync(before, s, new SymbolRenameOptions(), name, ct).ConfigureAwait(false);
            return new JsonObject { ["files"] = await Changes(before, after, ct).ConfigureAwait(false) };
        }

        async Task<JsonObject> Format(Document doc, CancellationToken ct)
        {
            Document formatted = await Formatter.FormatAsync(doc, cancellationToken: ct).ConfigureAwait(false);
            SourceText text = await doc.GetTextAsync(ct).ConfigureAwait(false);
            var changes = await formatted.GetTextChangesAsync(doc, ct).ConfigureAwait(false);
            var edits = new JsonArray();
            foreach (TextChange c in changes) edits.Add(Edit(text, c.Span.Start, c.Span.End, c.NewText));
            return new JsonObject { ["edits"] = edits };
        }

        // Per changed file: the edits against its current text, and the whole new
        // text (for files the editor does not have open).
        async Task<JsonArray> Changes(Solution before, Solution after, CancellationToken ct)
        {
            var files = new JsonArray();
            foreach (ProjectChanges pc in after.GetChanges(before).GetProjectChanges())
                foreach (DocumentId id in pc.GetChangedDocuments())
                {
                    Document a = before.GetDocument(id), b = after.GetDocument(id);
                    SourceText old = await a.GetTextAsync(ct).ConfigureAwait(false);
                    SourceText neu = await b.GetTextAsync(ct).ConfigureAwait(false);
                    var edits = new JsonArray();
                    foreach (TextChange c in await b.GetTextChangesAsync(a, ct).ConfigureAwait(false))
                        edits.Add(Edit(old, c.Span.Start, c.Span.End, c.NewText));
                    files.Add(new JsonObject { ["file"] = a.FilePath, ["edits"] = edits, ["text"] = neu.ToString() });
                }
            return files;
        }

        // --- Helpers -------------------------------------------------------------
        static int KindOf(ImmutableArray<string> tags)
        {
            foreach (string t in tags)
                switch (t)
                {
                    case WellKnownTags.Method: case WellKnownTags.ExtensionMethod: return 1;
                    case WellKnownTags.Property: return 2;
                    case WellKnownTags.Field: return 3;
                    case WellKnownTags.Event: return 4;
                    case WellKnownTags.Local: case WellKnownTags.Parameter: case WellKnownTags.RangeVariable: return 5;
                    case WellKnownTags.Class: return 6;
                    case WellKnownTags.Structure: return 7;
                    case WellKnownTags.Interface: return 8;
                    case WellKnownTags.Enum: return 9;
                    case WellKnownTags.EnumMember: return 10;
                    case WellKnownTags.Constant: return 11;
                    case WellKnownTags.Delegate: return 12;
                    case WellKnownTags.Namespace: return 13;
                    case WellKnownTags.Keyword: return 14;
                    case WellKnownTags.Snippet: return 15;
                    case WellKnownTags.TypeParameter: return 16;
                    case WellKnownTags.Label: return 17;
                    case WellKnownTags.Operator: return 18;
                }
            return 0;
        }

        static JsonObject Edit(SourceText text, int start, int end, string newText)
            => new JsonObject { ["start"] = Pos(text, start), ["end"] = Pos(text, end), ["text"] = newText };

        // {line, ch} -> offset, ch in code points.
        static int Offset(SourceText text, int line, int ch)
        {
            if (line < 0) return 0;
            if (line >= text.Lines.Count) return text.Length;
            TextLine l = text.Lines[line];
            int pos = l.Start;
            for (int i = 0; i < ch && pos < l.End; ++i)
                pos += char.IsHighSurrogate(text[pos]) && pos + 1 < l.End ? 2 : 1;
            return pos;
        }

        static JsonObject Pos(SourceText text, int offset)
        {
            offset = Math.Clamp(offset, 0, text.Length);
            TextLine l = text.Lines.GetLineFromPosition(offset);
            int ch = 0;
            for (int p = l.Start; p < offset; ++p)
                if (!char.IsLowSurrogate(text[p])) ++ch;
            return new JsonObject { ["line"] = l.LineNumber, ["ch"] = ch };
        }

        static int CharCount(SourceText text, TextLine l)
        {
            int n = 0;
            for (int p = l.Start; p < l.End; ++p) if (!char.IsLowSurrogate(text[p])) ++n;
            return n;
        }

        static string Lf(string s) => s.Replace("\r\n", "\n");

        static string Str(JsonObject o, string k) => o[k]?.GetValue<string>();
        static int Int(JsonObject o, string k) => o[k] == null ? 0 : (int)o[k].GetValue<double>();
    }
}

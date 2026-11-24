// Bigscreen Beta: films from folders of the Files app.
//
// Bigscreen lists its local films from /storage/emulated/0/Movies, which is
// Documents/android-files/Movies in this app's container; until now the only
// way in was copying with devicectl from a Mac. Here the player picks folders
// in the system's folder picker ("On My Apple Vision Pro", iCloud Drive, or any
// folder in them). Each is kept as a bookmark; at every start its access is
// opened for the whole run, an empty folder of the same name is made inside
// Movies so the game's listing shows it, and the runtime serves everything at
// and below that name from the picked folder (kl_guest_dir_aliases_set,
// runtime/libc/kl_libc.c). Nothing is copied, and the game's own Movies folder
// stays as it was. Bigscreen only: no other game offers this or registers
// anything, so their paths stay exactly as they were.
import Foundation
import SwiftUI
import UniformTypeIdentifiers

@MainActor
final class KLBigscreenFolders: ObservableObject {
    static let shared = KLBigscreenFolders()
    /// The one game that lists films from Movies.
    static var offered: Bool { klTargetName() == "x2497738113633933" }

    struct Folder: Codable, Identifiable, Equatable {
        var name: String        // the folder's name inside Movies
        var bookmark: Data
        var id: String { name }
    }

    private static let key = "bigscreen.filesFolders"
    @Published private(set) var folders: [Folder] = []
    /// Folders whose bookmark no longer resolves (deleted, or iCloud signed out).
    @Published private(set) var unreachable: Set<String> = []
    /// Picked URLs whose access is open; each is closed when its folder is removed.
    private var open: [String: URL] = [:]

    private init() {
        if let d = UserDefaults.standard.data(forKey: Self.key),
           let f = try? JSONDecoder().decode([Folder].self, from: d) {
            folders = f
        }
    }

    private static var movies: URL {
        URL(fileURLWithPath: Paths.container).appendingPathComponent("android-files/Movies")
    }

    private func save() {
        if let d = try? JSONEncoder().encode(folders) { UserDefaults.standard.set(d, forKey: Self.key) }
    }

    /// The folder's own name as the Files app shows it, made safe for a single
    /// path component and distinct from the other folders and from what the
    /// game's own Movies folder already holds.
    private func name(for url: URL) -> String {
        let shown = (try? url.resourceValues(forKeys: [.localizedNameKey]).localizedName) ?? url.lastPathComponent
        var base = shown.replacingOccurrences(of: "/", with: "-")
            .replacingOccurrences(of: ":", with: "-")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if base.isEmpty || base.hasPrefix(".") { base = klT("Folder", "Ordner") }
        let taken = Set(folders.map(\.name))
        let fm = FileManager.default
        func free(_ n: String) -> Bool {
            !taken.contains(n) && !fm.fileExists(atPath: Self.movies.appendingPathComponent(n).path)
        }
        if free(base) { return base }
        var i = 2
        while !free("\(base) \(i)") { i += 1 }
        return "\(base) \(i)"
    }

    /// A folder chosen in the picker. Returns a message for the player on failure.
    func add(_ url: URL) -> String? {
        let scoped = url.startAccessingSecurityScopedResource()
        guard let data = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) else {
            if scoped { url.stopAccessingSecurityScopedResource() }
            return klT("This folder cannot be kept. Choose another one.",
                       "Dieser Ordner lässt sich nicht merken. Wähle einen anderen.")
        }
        if let same = folders.first(where: { open[$0.name]?.standardizedFileURL == url.standardizedFileURL }) {
            if scoped { url.stopAccessingSecurityScopedResource() }
            return klT("\"\(same.name)\" is already there.", "„\(same.name)“ ist schon da.")
        }
        let f = Folder(name: name(for: url), bookmark: data)
        folders.append(f)
        open[f.name] = url
        save()
        NSLog("[bigscreen] Files folder added: \"\(f.name)\" -> \(url.path)")
        register()
        return nil
    }

    func remove(_ f: Folder) {
        folders.removeAll { $0.name == f.name }
        save()
        register()
        open.removeValue(forKey: f.name)?.stopAccessingSecurityScopedResource()
        unreachable.remove(f.name)
        // Only the empty stand-in folder; rmdir refuses anything that holds files.
        rmdir(Self.movies.appendingPathComponent(f.name).path)
        NSLog("[bigscreen] Files folder removed: \"\(f.name)\"")
    }

    /// Before the guest boots: open every kept folder and tell the runtime.
    func applyAtBoot() {
        guard Self.offered else { return }
        var changed = false
        for (i, f) in folders.enumerated() where open[f.name] == nil {
            var stale = false
            guard let u = try? URL(resolvingBookmarkData: f.bookmark, options: [], relativeTo: nil,
                                   bookmarkDataIsStale: &stale) else {
                unreachable.insert(f.name)
                NSLog("[bigscreen] Files folder \"\(f.name)\" cannot be reached")
                continue
            }
            _ = u.startAccessingSecurityScopedResource()
            open[f.name] = u
            unreachable.remove(f.name)
            if stale, let d = try? u.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
                folders[i].bookmark = d
                changed = true
            }
        }
        if changed { save() }
        register()
    }

    /// The runtime's list, and the stand-in folders the game's listing shows.
    private func register() {
        let fm = FileManager.default
        var lines: [String] = []
        for f in folders {
            guard let u = open[f.name] else { continue }
            try? fm.createDirectory(at: Self.movies.appendingPathComponent(f.name), withIntermediateDirectories: true)
            // An iCloud folder that is not on the headset yet lists nothing until it
            // is asked for (LauncherFiles.prepare found the same). Only the folders
            // are asked for here; a film downloads when the game reads it.
            kickFolders(u)
            lines.append("/Movies/\(f.name)\t\(u.path)")
        }
        kl_guest_dir_aliases_set(lines.isEmpty ? nil : lines.joined(separator: "\n"))
    }

    private nonisolated func kickFolders(_ root: URL) {
        Task.detached(priority: .utility) {
            let fm = FileManager.default
            guard (try? root.resourceValues(forKeys: [.isUbiquitousItemKey]).isUbiquitousItem) == true else { return }
            try? fm.startDownloadingUbiquitousItem(at: root)
            guard let en = fm.enumerator(at: root, includingPropertiesForKeys: [.isDirectoryKey],
                                         options: [.skipsHiddenFiles]) else { return }
            var n = 0
            while let u = en.nextObject() as? URL, n < 200 {
                if (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                    try? fm.startDownloadingUbiquitousItem(at: u)
                    n += 1
                }
            }
        }
    }
}

struct BigscreenFoldersView: View {
    @ObservedObject private var store = KLBigscreenFolders.shared
    @State private var picking = false
    @State private var message = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Image(systemName: "film")
                Text("Movies").bold()
                Spacer()
                Text(klT("the game's own folder", "eigener Ordner des Spiels"))
                    .font(.caption).foregroundStyle(.secondary)
            }
            ForEach(store.folders) { f in
                HStack {
                    Image(systemName: store.unreachable.contains(f.name) ? "exclamationmark.triangle" : "folder")
                    VStack(alignment: .leading, spacing: 2) {
                        Text(f.name)
                        if store.unreachable.contains(f.name) {
                            Text(klT("Cannot be reached. Remove it and choose it again.",
                                     "Nicht erreichbar. Entfernen und neu wählen."))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    Spacer()
                    Button(role: .destructive) { store.remove(f) } label: {
                        Label(klT("Remove", "Entfernen"), systemImage: "minus.circle")
                    }
                    .labelStyle(.iconOnly)
                }
            }
            Button { message = ""; picking = true } label: {
                Label(klT("Add a folder…", "Ordner hinzufügen …"), systemImage: "folder.badge.plus")
            }
            Text(klT("Bigscreen shows each folder inside Movies. Removing one here deletes nothing in the Files app.",
                     "Bigscreen zeigt jeden Ordner in Movies. Hier entfernen löscht nichts in der Dateien-App."))
                .font(.caption).foregroundStyle(.secondary)
            if !message.isEmpty { Text(message).font(.footnote) }
        }
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { r in
            switch r {
            case .success(let u): message = store.add(u) ?? klT("Added. Bigscreen shows it the next time it starts.",
                                                                  "Hinzugefügt. Bigscreen zeigt ihn beim nächsten Start.")
            case .failure(let e): message = klT("No folder chosen: \(e.localizedDescription)",
                                                 "Kein Ordner gewählt: \(e.localizedDescription)")
            }
        }
    }
}

// Lewd Vault's share extension: "Lewd Vault" in the share sheet (Photos, Safari, any app sharing a
// picture or video). It copies what was shared into the app group's share-inbox folder and closes;
// the app adds it to the vault the next time it comes to the front (src-tauri/src/phone.rs, "inbox").
// Added to the Xcode project by tools/ios-patch.mjs (the project itself is generated in CI).
import UIKit
import UniformTypeIdentifiers

/// The app group shared with the app. A sideloading tool may rename it (adding its team ID), so the
/// one this build was actually signed with is read from the provisioning profile when there is one.
func lvGroupDir() -> URL? {
    var ids = ["group.local.imagevault.desktop"]
    if let url = Bundle.main.url(forResource: "embedded", withExtension: "mobileprovision"),
       let data = try? Data(contentsOf: url), let text = String(data: data, encoding: .isoLatin1),
       let key = text.range(of: "<key>com.apple.security.application-groups</key>"),
       let open = text.range(of: "<string>", range: key.upperBound..<text.endIndex),
       let close = text.range(of: "</string>", range: open.upperBound..<text.endIndex) {
        ids.insert(String(text[open.upperBound..<close.lowerBound]), at: 0)
    }
    for id in ids {
        if let dir = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: id) { return dir }
    }
    return nil
}

class ShareViewController: UIViewController {
    private let label = UILabel()

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UIColor(red: 0.16, green: 0.12, blue: 0.14, alpha: 0.96)
        label.text = "Adding to Lewd Vault…"
        label.textColor = .white
        label.font = .systemFont(ofSize: 17, weight: .semibold)
        label.textAlignment = .center
        label.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(label)
        NSLayoutConstraint.activate([label.centerXAnchor.constraint(equalTo: view.centerXAnchor), label.centerYAnchor.constraint(equalTo: view.centerYAnchor)])
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        Task {
            let n = await take()
            label.text = n > 0 ? (n == 1 ? "Added. Open Lewd Vault to see it." : "\(n) added. Open Lewd Vault to see them.") : "Nothing that Lewd Vault can keep."
            try? await Task.sleep(nanoseconds: 900_000_000)
            extensionContext?.completeRequest(returningItems: nil)
        }
    }

    private func take() async -> Int {
        guard let base = lvGroupDir() else { return 0 }
        let dir = base.appendingPathComponent("share-inbox", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        var n = 0
        let items = (extensionContext?.inputItems as? [NSExtensionItem]) ?? []
        for item in items {
            for provider in item.attachments ?? [] {
                for type in [UTType.movie, UTType.image] where provider.hasItemConformingToTypeIdentifier(type.identifier) {
                    if await copy(provider, type, into: dir, index: n) { n += 1 }
                    break
                }
            }
        }
        return n
    }

    /// The file itself when the app shares one; otherwise the picture's data (a photo as JPEG).
    private func copy(_ provider: NSItemProvider, _ type: UTType, into dir: URL, index: Int) async -> Bool {
        let stamp = "\(Int(Date().timeIntervalSince1970 * 1000))-\(index)"
        let fromFile: Bool = await withCheckedContinuation { done in
            provider.loadFileRepresentation(forTypeIdentifier: type.identifier) { url, _ in
                guard let url else { return done.resume(returning: false) }
                let name = url.lastPathComponent.replacingOccurrences(of: "/", with: "_")
                let dest = dir.appendingPathComponent("\(stamp)-\(name)")
                done.resume(returning: (try? FileManager.default.copyItem(at: url, to: dest)) != nil)
            }
        }
        if fromFile { return true }
        if type == .image {
            let data: Data? = await withCheckedContinuation { done in
                provider.loadDataRepresentation(forTypeIdentifier: type.identifier) { data, _ in done.resume(returning: data) }
            }
            if let data, let image = UIImage(data: data), let jpeg = image.jpegData(compressionQuality: 0.95) {
                return (try? jpeg.write(to: dir.appendingPathComponent("\(stamp)-shared.jpg"))) != nil
            }
        }
        return false
    }
}

package dev.curiositystack.a11.clion.tools

import com.intellij.openapi.editor.Document
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.io.FileUtil
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.psi.PsiFile
import com.intellij.psi.PsiManager
import com.intellij.psi.search.FilenameIndex
import com.intellij.psi.search.GlobalSearchScope

/**
 * A file something works on: its PSI, the document its text lives in, and the
 * virtual file both come from.
 *
 * [path] is the absolute path, which is what every IDE tool reports and what a
 * caller hands back when it means "that file again".
 */
internal class TargetFile(val psiFile: PsiFile, val document: Document, val file: VirtualFile) {
    val path: String get() = file.path
}

/**
 * Turning a path a caller wrote into a file this project can work on.
 *
 * Extracted from [IdeTools] because the editor-side suggestion popup resolves
 * paths too, and it has to resolve them *the same way*: a comment the flow
 * attached to a path has to land on the file that path named when the flow read
 * it, whichever of the two spellings below it used.
 */
internal object ProjectFiles {

    /**
     * Resolve a requested path to a file to work on. Call under a read action.
     *
     * Both spellings a caller is likely to have are accepted: the absolute path the
     * IDE tools report, and the project-relative one a human reads.
     */
    fun resolve(project: Project, path: String): TargetFile {
        val requested = FileUtil.toSystemIndependentName(path)
        val file: VirtualFile = onLocalDisk(project, requested)
            ?: inProjectIndex(project, requested)
            ?: throw IllegalArgumentException("No file at '$path' in this project.")
        require(!file.isDirectory) { "'$path' is a directory, not a file." }
        val document = FileDocumentManager.getInstance().getDocument(file)
            ?: throw IllegalArgumentException("'$path' has no text to analyze (a binary file?).")
        val psiFile = PsiManager.getInstance(project).findFile(file)
            ?: throw IllegalArgumentException("'$path' is not part of this project, so it is not analyzed.")
        return TargetFile(psiFile, document, file)
    }

    /** The file [requested] names on disk: as given, or relative to the project root. */
    private fun onLocalDisk(project: Project, requested: String): VirtualFile? {
        val fileSystem = LocalFileSystem.getInstance()
        fileSystem.findFileByPath(requested)?.let { return it }
        val base = project.basePath?.takeIf { !FileUtil.isAbsolute(requested) } ?: return null
        return fileSystem.findFileByPath("$base/${requested.trimStart('/')}")
    }

    /**
     * The project file whose path ends with [requested], via the filename index.
     *
     * This is what reaches a file the local filesystem knows nothing about — a
     * project hosted remotely or in a container, or a test fixture's in-memory
     * VFS. An ambiguous suffix is reported with the candidates rather than
     * resolved by picking one.
     */
    private fun inProjectIndex(project: Project, requested: String): VirtualFile? {
        val name = requested.substringAfterLast('/')
        if (name.isEmpty()) return null
        val suffix = "/${requested.trimStart('/')}"
        val matches = FilenameIndex.getVirtualFilesByName(name, GlobalSearchScope.projectScope(project))
            .filter { it.path == requested || it.path.endsWith(suffix) }
        require(matches.size <= 1) {
            "'$requested' matches ${matches.size} files (${matches.joinToString { it.path }}); " +
                "pass an absolute path to say which one."
        }
        return matches.firstOrNull()
    }
}

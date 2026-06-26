/**
 * @file editor.h
 * @brief Vix 4.0 - Modern C Editor interface.
 *
 * External interface for the editor. The shell calls start_editor() to
 * launch it; the keyboard input loop routes keys to editor_input() while
 * editor_running is 1.
 */

#ifndef EDITOR_H
#define EDITOR_H

/**
 * @brief Initialize and start the text editor for a specific file.
 * @param filename The path to the file to open or create.
 */
void start_editor(const char* filename);

#endif // EDITOR_H

/**
 * @file editor.h
 * @brief Vim-like Text Editor (Vix) interface.
 */

#ifndef EDITOR_H
#define EDITOR_H

/**
 * @brief Initialize and start the text editor for a specific file.
 * @param filename The path to the file to open or create.
 */
void start_editor(const char* filename);

#endif // EDITOR_H

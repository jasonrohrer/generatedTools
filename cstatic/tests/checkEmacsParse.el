;;; checkEmacsParse.el --- verify cstatic output is parsed by M-x compile
;;
;; Feeds a captured cstatic run through the same compilation-mode machinery
;; that "M-x compile" uses, and prints every location emacs found.
;;
;; Usage:
;;     emacs -Q --batch -l tests/checkEmacsParse.el -f cstatic-check <output-file>
;;
;; Every line cstatic meant as a finding should show up exactly once, and no
;; progress or explanation line should show up at all.

(require 'compile)

(defun cstatic-check ()
  (let ((file (car command-line-args-left))
        (found '()))
    (unless file
      (error "usage: emacs -Q --batch -l checkEmacsParse.el -f cstatic-check FILE"))
    (with-current-buffer (get-buffer-create "*cstatic test*")
      (insert-file-contents file)
      (goto-char (point-max))
      ;; compilation-mode parses on the fly as output arrives; in batch we
      ;; turn it on and ask it to parse the whole buffer.
      (let ((inhibit-read-only t))
        (compilation-mode)
        (compilation--ensure-parse (point-max)))
      (goto-char (point-min))
      (let ((pos (point-min)))
        (while (setq pos (next-single-property-change pos 'compilation-message))
          (let ((msg (get-text-property pos 'compilation-message)))
            (when msg
              (let* ((loc (compilation--message->loc msg))
                     (line (compilation--loc->line loc))
                     (col (compilation--loc->col loc))
                     (fs (compilation--loc->file-struct loc))
                     (name (caar fs))
                     (type (compilation--message->type msg))
                     (text (buffer-substring-no-properties
                            (line-beginning-position)
                            (line-end-position))))
                (push (list name line col type text) found))))
          (goto-char pos)))
      (setq found (nreverse (delete-dups found)))
      (princ (format "parsed %d location(s) from %s\n\n" (length found) file))
      (dolist (f found)
        (princ (format "  %-14s line %-5s col %-5s type %s\n"
                       (nth 0 f) (nth 1 f) (or (nth 2 f) "-")
                       (pcase (nth 3 f) (0 "info") (1 "warning") (_ "error")))))
      (princ "\n")
      ;; sanity: every "file:line:col:" line in the raw text should have been
      ;; parsed, and nothing else should have been.
      (let ((raw 0))
        (goto-char (point-min))
        (while (re-search-forward
                "^[^ \t\n:]+\\.[ch]:[0-9]+:[0-9]+: \\(error\\|warning\\):" nil t)
          (setq raw (1+ raw)))
        (princ (format "gcc-style finding lines in file: %d\n" raw))
        (princ (format "locations emacs can jump to:     %d\n" (length found)))
        (if (= raw (length found))
            (princ "RESULT: OK -- emacs parsed exactly the finding lines\n")
          (princ "RESULT: MISMATCH -- check for stray parseable lines\n"))))))

(provide 'checkEmacsParse)

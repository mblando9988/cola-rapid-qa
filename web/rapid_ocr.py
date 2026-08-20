#!/usr/bin/env python3
"""rapid_ocr.py — RapidOCR OCR + the COLA label matching logic.

This replaces the C++ binary's Tesseract OCR stage. The C++ binary still does
stage 1 (MuPDF: extract page text, parse the page-1 form Q&A, and dump embedded
label images). This module then:

  1. runs RapidOCR (PaddleOCR models on ONNX Runtime) over each extracted image,
  2. groups the detected text boxes into lines,
  3. runs the *same* matching logic as cola_label_qa.cpp (norm / field_kind /
     match_answer / match_class_type), so verdicts are identical to the
     Tesseract path but ~3x faster.

The matching code below is a line-for-line port of the C++ stage 2. Do not
"improve" it — the point is bit-identical verdicts.
"""

import re
from rapidocr_onnxruntime import RapidOCR

# ---------------------------------------------------------------------------
# RapidOCR engine (loaded once, reused across requests)
# ---------------------------------------------------------------------------

_engine = None


def get_engine():
    global _engine
    if _engine is None:
        _engine = RapidOCR()
    return _engine


def engine_ready():
    """Return whether the shared OCR engine completed initialization."""
    return _engine is not None


def ocr_lines(path):
    """OCR one image file -> list of line dicts {text, conf, x1, y1, x2, y2}."""
    out = get_engine()(path)
    result = out[0] if isinstance(out, tuple) else out
    lines = []
    if not result:
        return lines
    for item in result:
        box, text, score = item[0], item[1], float(item[2])
        text = (text or "").strip()
        if not text:
            continue
        xs = [float(p[0]) for p in box]
        ys = [float(p[1]) for p in box]
        lines.append({
            "text": text,
            "conf": score,
            "x1": int(min(xs)), "y1": int(min(ys)),
            "x2": int(max(xs)), "y2": int(max(ys)),
        })
    return lines


# ---------------------------------------------------------------------------
# norm() — port of the C++ utf-8 aware normalizer
# ---------------------------------------------------------------------------

_DIACRITIC = {
    0xC0: "A", 0xC1: "A", 0xC2: "A", 0xC3: "A", 0xC4: "A", 0xC5: "A",
    0xC6: "AE", 0xC7: "C", 0xC8: "E", 0xC9: "E", 0xCA: "E", 0xCB: "E",
    0xCC: "I", 0xCD: "I", 0xCE: "I", 0xCF: "I", 0xD0: "D", 0xD1: "N",
    0xD2: "O", 0xD3: "O", 0xD4: "O", 0xD5: "O", 0xD6: "O", 0xD7: "",
    0xD8: "O", 0xD9: "U", 0xDA: "U", 0xDB: "U", 0xDC: "U", 0xDD: "Y",
    0xDE: "TH", 0xDF: "ss", 0xE0: "a", 0xE1: "a", 0xE2: "a", 0xE3: "a",
    0xE4: "a", 0xE5: "a", 0xE6: "ae", 0xE7: "c", 0xE8: "e", 0xE9: "e",
    0xEA: "e", 0xEB: "e", 0xEC: "i", 0xED: "i", 0xEE: "i", 0xEF: "i",
    0xF0: "d", 0xF1: "n", 0xF2: "o", 0xF3: "o", 0xF4: "o", 0xF5: "o",
    0xF6: "o", 0xF7: "", 0xF8: "o", 0xF9: "u", 0xFA: "u", 0xFB: "u",
    0xFC: "u", 0xFD: "y", 0xFE: "th", 0xFF: "y",
}


def norm(s):
    out = []
    last_space = True
    for ch in s:
        cp = ord(ch)
        if cp < 0x80:
            c = ch
            if "A" <= c <= "Z":
                c = chr(ord(c) - ord("A") + ord("a"))
            alnum = ("a" <= c <= "z") or ("0" <= c <= "9")
            if alnum:
                out.append(c)
                last_space = False
            elif not last_space:
                out.append(" ")
                last_space = True
            continue
        if 0x0300 <= cp <= 0x036F:  # combining marks
            continue
        if 0x00C0 <= cp <= 0x00FF:  # accented latin
            base = _DIACRITIC.get(cp)
            if base:
                out.append(base)
                last_space = False
            continue
        # other scripts: letter placeholder
        if not last_space:
            out.append(" ")
        out.append("a")
        last_space = True
    return "".join(out).rstrip(" ")


def compact_norm(s):
    return norm(s).replace(" ", "")


# ---------------------------------------------------------------------------
# matching — port of match_answer + match_class_type
# ---------------------------------------------------------------------------

def field_kind(question):
    q = question.lower()
    tab = [
        ("show any wording", "Wording"),
        ("show any information", "Wording"),
        ("brand name", "Brand"),
        ("fanciful name", "Fanciful"),
        ("net contents", "NetContents"),
        ("alcohol content", "Alcohol"),
        ("appellation", "Appellation"),
        ("vintage", "Vintage"),
        ("type of product", "ProductType"),
        ("name and address", "NameAddress"),
    ]
    for key, kind in tab:
        if key in q:
            return kind
    return "Admin"


def tagged_corpus(image_lines):
    """image_lines: list of (image_name, [line dicts]) -> flat tagged list."""
    out = []
    for name, lines in image_lines:
        for l in lines:
            out.append((name, l))
    return out


def find_in_corpus(tl, pred):
    for name, l in tl:
        if pred(norm(l["text"])):
            return {
                "found": True,
                "image": name,
                "line": l["text"],
                "conf": l["conf"],
                "line_x1": l["x1"], "line_y1": l["y1"],
                "line_x2": l["x2"], "line_y2": l["y2"],
            }
    return {
        "found": False,
        "image": "", "line": "", "conf": -1,
        "line_x1": 0, "line_y1": 0, "line_x2": 0, "line_y2": 0,
    }


def full_match(answer, tl):
    a = norm(answer)
    if not a:
        return find_in_corpus(tl, lambda nl: False)
    return find_in_corpus(tl, lambda nl: a in nl)


def full_match_compact(answer, tl):
    compact = compact_norm(answer)
    if not compact:
        return find_in_corpus(tl, lambda nl: False)
    return find_in_corpus(tl, lambda nl: compact in nl.replace(" ", ""))


def token_match(answer, tl, min_len, need):
    toks = [t for t in norm(answer).split() if len(t) >= min_len]
    if not toks:
        return find_in_corpus(tl, lambda nl: False)
    same_line = find_in_corpus(tl, lambda nl: all(t in nl for t in toks))
    if same_line["found"]:
        same_line["found_n"] = len(toks)
        same_line["of_n"] = len(toks)
        return same_line
    found = []
    for t in toks:
        e = find_in_corpus(tl, lambda nl, t=t: t in nl)
        if e["found"]:
            found.append(t)
    e = {
        "found": False, "image": "", "line": "", "conf": -1,
        "line_x1": 0, "line_y1": 0, "line_x2": 0, "line_y2": 0,
        "found_n": len(found), "of_n": len(toks),
    }
    if len(found) / len(toks) >= need:
        e["found"] = True
        e["image"] = "multiple"
        e["line"] = " ".join(found)
    return e


_GOVERNMENT_WARNING_ANCHORS = [
    "governmentwarning",
    "surgeongeneral",
    "womenshouldnotdrink",
    "pregnancy",
    "birthdefects",
    "consumptionofalcoholicbeverages",
    "abilitytodrive",
    "healthproblems",
]


def match_government_warning(tl):
    corpus = "".join(compact_norm(line["text"]) for _, line in tl)
    found_n = sum(anchor in corpus for anchor in _GOVERNMENT_WARNING_ANCHORS)
    total = len(_GOVERNMENT_WARNING_ANCHORS)
    evidence = find_in_corpus(
        tl, lambda line: "government" in line and "warning" in line)
    if not evidence["found"] and found_n:
        evidence = find_in_corpus(
            tl, lambda line: any(anchor in line.replace(" ", "")
                                 for anchor in _GOVERNMENT_WARNING_ANCHORS))
    evidence["found_n"] = found_n
    evidence["of_n"] = total

    if found_n == total:
        status = "MATCH"
        message = f"SEEN - government warning complete ({found_n}/{total} checks)"
    elif found_n:
        status = "PARTIAL"
        message = f"INCOMPLETE - government warning ({found_n}/{total} checks)"
    else:
        status = "NOT FOUND"
        message = "NOT SEEN - government warning is mandatory"

    return {
        "num": "GW",
        "question": "GOVERNMENT WARNING (100% REQUIRED)",
        "answer": "100% REQUIRED",
        "status": status,
        "evidence": evidence,
        "note": "mandatory government warning check",
        "mandatory": True,
        "mandatory_message": message,
    }


def match_answer(kind, question, answer, tl):
    r = {"question": question, "answer": answer}
    a = norm(answer)

    if kind == "NetContents":
        ev = full_match(answer, tl)
        if not ev["found"]:
            m = re.search(r"(\d+(?:\.\d+)?)", answer)
            if m:
                vol = m.group(1)
                pat = re.compile(
                    r"\b" + vol + r"(?:\.0+)?\s*m[l|i|!|1]\b"
                    r"|\b" + vol + r"(?:\.0+)?\s*millilit(?:er|re)s?\b",
                    re.IGNORECASE)
                ev = find_in_corpus(tl, lambda raw: pat.search(raw) is not None)
        r["status"] = "MATCH" if ev["found"] else "NOT FOUND"
        r["evidence"] = ev
        return r

    if kind == "Alcohol":
        m = re.search(r"(\d+(?:\.\d+)?)", answer)
        if m:
            n = m.group(1)
            pat = re.compile(
                r"\b" + n + r"(?:\.0+)?\s*0?\s*(?:%|percent|per cent|alc\.?|alcohol|by vol(?:ume)?)"
                r"|(?:alc\.?|alcohol)[.:]?\s*\b" + n + r"(?:\.0+)?\b",
                re.IGNORECASE)
            ev = find_in_corpus(tl, lambda raw: pat.search(raw) is not None)
        else:
            ev = full_match(answer, tl)
        r["status"] = "MATCH" if ev["found"] else "NOT FOUND"
        r["evidence"] = ev
        return r

    if kind == "ProductType":
        u = answer.lower()
        cands = []
        if "wine" in u:
            cands.append(("vino", "wine"))
            cands.append(("wine", "wine"))
        if "distilled" in u:
            cands.append(("distilled spirits", "distilled spirits"))
        if "malt" in u:
            cands.append(("malt beverage", "malt beverage"))
        for cand_text, cand_as in cands:
            ev = full_match(cand_text, tl)
            if ev["found"]:
                r["status"] = "MATCH"
                r["evidence"] = ev
                return r
        r["status"] = "NOT FOUND"
        r["evidence"] = find_in_corpus(tl, lambda nl: False)
        return r

    if kind == "Wording":
        toks = [t for t in a.split() if len(t) >= 4]
        if not toks:
            r["status"] = "EMPTY"
            r["evidence"] = find_in_corpus(tl, lambda nl: False)
            return r
        found = []
        for t in toks:
            e = find_in_corpus(tl, lambda nl, t=t: t in nl)
            if e["found"]:
                found.append(t)
        cov = len(found) / len(toks)
        r["status"] = "MATCH" if cov >= 0.9 else ("PARTIAL" if cov >= 0.5 else "NOT FOUND")
        ev = {
            "found": True, "image": "multiple",
            "line": " ".join(found), "conf": -1,
            "line_x1": 0, "line_y1": 0, "line_x2": 0, "line_y2": 0,
            "found_n": len(found), "of_n": len(toks),
        }
        r["evidence"] = ev
        return r

    if kind == "NameAddress":
        idx = answer.lower().rfind("(used on label)")
        if idx == -1:
            r["status"] = "SKIP"
            r["note"] = "no tradename marked 'Used on label'"
            r["evidence"] = find_in_corpus(tl, lambda nl: False)
            return r
        toks = answer[:idx].split()
        for n in range(4, 0, -1):
            if len(toks) < n:
                continue
            dba = " ".join(toks[len(toks) - n:]).strip()
            while dba and dba[-1] in ",.":
                dba = dba[:-1]
            ev = full_match(dba, tl)
            if not ev["found"]:
                ev = full_match_compact(dba, tl)
            if ev["found"]:
                r["status"] = "MATCH"
                r["evidence"] = ev
                return r
        r["status"] = "NOT FOUND"
        r["evidence"] = find_in_corpus(tl, lambda nl: False)
        return r

    if kind == "Admin":
        r["status"] = "SKIP"
        r["note"] = "administrative field — not printed on label"
        r["evidence"] = find_in_corpus(tl, lambda nl: False)
        return r

    # brand / fanciful / appellation / vintage
    ev = full_match(answer, tl)
    if ev["found"]:
        r["status"] = "MATCH"
        r["evidence"] = ev
        return r
    ev = token_match(answer, tl, 3, 1.0)
    if ev["found"]:
        r["status"] = "MATCH" if ev["found_n"] == ev["of_n"] else "PARTIAL"
        r["evidence"] = ev
        return r
    r["status"] = "NOT FOUND"
    r["evidence"] = find_in_corpus(tl, lambda nl: False)
    return r


def match_class_type(tl, page2):
    r = {"status": "SKIP", "class_type": "", "matched_as": ""}
    m = re.search(r"CLASS/TYPE DESCRIPTION\s*\n([^\n]+)", page2, re.IGNORECASE)
    if not m:
        return r
    class_type = m.group(1).strip()
    r["class_type"] = class_type
    nct = norm(class_type)
    variants = [nct]
    words = nct.split()
    if len(words) == 3 and words[0] == "table" and words[1] == "red" and words[2] == "wine":
        variants += ["red wine", "red table wine", "vino rosso da tavola", "vino rosso"]
    for v in variants:
        ev = full_match(v, tl)
        if not ev["found"]:
            ev = full_match_compact(v, tl)
        if ev["found"]:
            r["status"] = "MATCH"
            r["matched_as"] = v
            return r
    r["status"] = "NOT FOUND"
    return r


# ---------------------------------------------------------------------------
# top-level: OCR images + match against form Q&A
# ---------------------------------------------------------------------------

def ocr_and_match(form_data):
    """form_data: the C++ --task form --json dict.
    Returns {"ocr": {...}, "matches": [...], "class_type": {...}}."""
    images = form_data.get("images", [])
    image_lines = []
    ocr = {}
    for im in images:
        name = im.get("name")
        path = im.get("path")
        lines = ocr_lines(path) if path else []
        image_lines.append((name, lines))
        ocr[name] = {
            "width": im.get("width", 0),
            "height": im.get("height", 0),
            "lines": [{"text": l["text"], "conf": l["conf"]} for l in lines],
            "text": " ".join(l["text"] for l in lines),
        }

    tl = tagged_corpus(image_lines)

    matches = []
    for e in form_data.get("form_qa", []):
        answer = (e.get("answer") or "").strip()
        if not answer:
            continue
        kind = field_kind(e.get("question") or "")
        m = match_answer(kind, e.get("question") or "", answer, tl)
        m["num"] = e.get("num", "")
        m["question"] = e.get("question", "")
        m["answer"] = answer
        if isinstance(e.get("bbox"), dict):
            m["form_bbox"] = e["bbox"]
        m.setdefault("note", "")
        matches.append(m)

    matches.append(match_government_warning(tl))

    raw_text = form_data.get("raw_text", [])
    page2 = raw_text[1] if len(raw_text) > 1 else ""
    class_type = match_class_type(tl, page2)

    return {"ocr": ocr, "matches": matches, "class_type": class_type}

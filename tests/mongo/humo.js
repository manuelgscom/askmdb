// Deterministic smoke databases for askmdb's e2e tests.
//
//   mongosh --quiet mongodb://127.0.0.1:27017 tests/mongo/humo.js
//
// Idempotent: drops and recreates the databases below. Deterministic: a fixed-seed LCG (never
// Math.random) and fixed dates (never new Date() without arguments), so two runs produce the same
// documents, the same _id values and the same sampled schema.
//
//   humo         pedidos (300), clientes (120), productos (80), ancha (50), view pedidos_madrid
//   humo_grande  10 collections of 40 long top-level fields: the block does not fit the budget;
//                valores: fits only after trimming its value lists
//   humo_muchas  51 tiny collections: more than the 50 sampled without -c

(function () {
  "use strict";

  // --- Deterministic helpers ------------------------------------------------------------------
  let state = 20251025;
  function next() {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state;
  }
  // Uniform integer in [0, n), from the high bits (the low bits of an LCG have short periods).
  function rand(n) {
    return Math.floor((next() / 4294967296) * n);
  }
  function pick(list) {
    return list[rand(list.length)];
  }
  function chance(percent) {
    return rand(100) < percent;
  }
  function weighted(pairs) {  // [[value, weight], ...]
    let total = 0;
    for (const p of pairs) total += p[1];
    let r = rand(total);
    for (const p of pairs) {
      if (r < p[1]) return p[0];
      r -= p[1];
    }
    return pairs[pairs.length - 1][0];
  }
  function pad(n, width) {
    return String(n).padStart(width, "0");
  }
  function hex(n, width) {
    return n.toString(16).padStart(width, "0");
  }
  // ObjectId whose timestamp is `seconds`; the rest is a fixed tag and a counter.
  function oid(seconds, tag, counter) {
    return ObjectId(hex(seconds, 8) + tag + hex(counter, 8));
  }
  function cents(c) {
    return Double(c / 100);
  }
  function decimal(c) {
    return NumberDecimal((c / 100).toFixed(2));
  }
  const DAY = 24 * 3600 * 1000;
  const T2025 = Date.UTC(2025, 0, 1);
  const T2026_10 = Date.UTC(2026, 9, 1);

  // --- humo -------------------------------------------------------------------------------
  const humo = db.getSiblingDB("humo");
  humo.dropDatabase();

  const cities = ["Madrid", "Barcelona", "Valencia", "Sevilla", "Bilbao"];
  const postal = { Madrid: 28000, Barcelona: 8000, Valencia: 46000, Sevilla: 41000, Bilbao: 48000 };

  // clientes: _id C-0001..C-0120, Spanish and English names. Email and phone are personal-looking
  // on purpose: the schema block must never list their values.
  const firstEs = [["Lucía", "lucia"], ["Javier", "javier"], ["Carmen", "carmen"], ["Andrés", "andres"],
                   ["María", "maria"], ["José", "jose"], ["Elena", "elena"], ["Íñigo", "inigo"],
                   ["Nuria", "nuria"], ["Raúl", "raul"]];
  const lastEs = [["García", "garcia"], ["Martínez", "martinez"], ["López", "lopez"], ["Sánchez", "sanchez"],
                  ["Pérez", "perez"], ["Gómez", "gomez"], ["Fernández", "fernandez"], ["Ruiz", "ruiz"]];
  const firstEn = [["Emma", "emma"], ["Oliver", "oliver"], ["Grace", "grace"], ["Harry", "harry"],
                   ["Chloe", "chloe"], ["Jack", "jack"]];
  const lastEn = [["Smith", "smith"], ["Taylor", "taylor"], ["Brown", "brown"], ["Wilson", "wilson"],
                  ["Evans", "evans"]];

  const clientes = [];
  for (let i = 1; i <= 120; i++) {
    const english = chance(30);
    const f = pick(english ? firstEn : firstEs);
    const l = pick(english ? lastEn : lastEs);
    const id = "C-" + pad(i, 4);
    const telefono = english
      ? "+44 7700 900" + pad(rand(1000), 3)
      : "+34 6" + pad(rand(100), 2) + " " + pad(rand(1000), 3) + " " + pad(rand(1000), 3);
    clientes.push({
      _id: id,
      nombre: f[0] + " " + l[0],
      email: f[1] + "." + l[1] + pad(i, 4) + "@correo.test",
      telefono: telefono,
      ciudad: pick(cities),
      alta: new Date(Date.UTC(2019, 0, 1) + rand(6 * 365) * DAY + rand(86400) * 1000),
      vip: chance(15),
    });
  }
  humo.clientes.insertMany(clientes);

  // productos: _id SKU-0001..SKU-0080.
  const nouns = ["Taza", "Lámpara", "Cuaderno", "Mochila", "Sartén", "Maceta", "Camiseta", "Auriculares",
                 "Bolígrafo", "Manta"];
  const adjectives = ["clásica", "de viaje", "ecológica", "plegable", "premium", "básica", "de diseño", "infantil"];
  const categories = ["cocina", "hogar", "jardin", "oficina", "regalo", "electronica", "deporte", "textil"];
  const productos = [];
  const priceCents = {};
  for (let i = 1; i <= 80; i++) {
    const sku = "SKU-" + pad(i, 4);
    const price = 299 + rand(15000);
    priceCents[sku] = price;
    const cats = [];
    const ncat = 1 + rand(3);
    while (cats.length < ncat) {
      const c = pick(categories);
      if (cats.indexOf(c) < 0) cats.push(c);
    }
    productos.push({
      _id: sku,
      nombre: pick(nouns) + " " + pick(adjectives) + " " + pad(i, 2),
      precio: decimal(price),
      categorias: cats,
      stock: NumberInt(rand(501)),
      descuento: chance(40) ? null : Double((5 + rand(26)) / 100),
    });
  }
  humo.productos.insertMany(productos);

  // pedidos: 300, dates spread over 2025-01-01..2026-10-01; _id timestamps follow the date, so
  // sort({_id:-1}) gives the most recent orders. cliente.{nombre,ciudad} match clientes.
  const span = Math.floor((T2026_10 - T2025) / 1000);
  const times = [];
  for (let i = 0; i < 300; i++) times.push(Math.floor(T2025 / 1000) + rand(span));
  times.sort(function (a, b) { return a - b; });
  const coupons = ["BIENVENIDA10", "VERANO25", "BLACKFRIDAY", "FIDELIDAD5"];
  const pedidos = [];
  for (let i = 0; i < 300; i++) {
    const c = clientes[rand(clientes.length)];
    const items = [];
    let total = 0;
    const nitems = 1 + rand(4);
    for (let k = 0; k < nitems; k++) {
      const sku = "SKU-" + pad(1 + rand(80), 4);
      const qty = 1 + rand(5);
      total += qty * priceCents[sku];
      items.push({ sku: sku, cantidad: NumberInt(qty), precio: cents(priceCents[sku]) });
    }
    const doc = {
      _id: oid(times[i], "0a5c0de0", i + 1),
      fecha: new Date(times[i] * 1000),
      cliente_id: c._id,
      cliente: { nombre: c.nombre, ciudad: c.ciudad },
      importe: cents(total),
      estado: weighted([["entregado", 50], ["enviado", 20], ["pendiente", 20], ["cancelado", 10]]),
      canal: weighted([["web", 50], ["tienda", 30], ["app", 20]]),
      items: items,
    };
    if (chance(30)) doc.cupon = pick(coupons);
    if (chance(70)) {
      const cp = postal[c.ciudad] + 1 + rand(50);
      doc.codigo_postal = chance(50) ? NumberInt(cp) : pad(cp, 5);  // int|str on purpose
    }
    pedidos.push(doc);
  }
  humo.pedidos.insertMany(pedidos);

  humo.createView("pedidos_madrid", "pedidos", [{ $match: { "cliente.ciudad": "Madrid" } }]);

  // ancha: 50 documents; a 5-level object n1.n2.n3.n4.n5 first (depth cap 4), then 60 top-level
  // fields f01..f60 (path cap 40). f02, f05, ... are strings with 10 frequent values each, long
  // enough to push the whole humo block over the budget, so trimming is exercised. Small but
  // enough for a deliberately slow read-only pipeline ($range + $unwind) with max_time_ms = 1.
  const ancha = [];
  const tAncha = Math.floor(Date.UTC(2026, 0, 1) / 1000);
  for (let i = 0; i < 50; i++) {
    const doc = {
      _id: oid(tAncha + i, "0a2c4a00", i + 1),
      n1: { n2: { n3: { n4: { n5: NumberInt(rand(1000)) } } } },
    };
    for (let j = 1; j <= 60; j++) {
      const name = "f" + pad(j, 2);
      if (j % 3 === 1) doc[name] = NumberInt(rand(10000));
      else if (j % 3 === 2) doc[name] = "valor-ancho-" + pad(j, 2) + "-" + pad(rand(10), 2);
      else doc[name] = Double(rand(1000000) / 100 + 0.25);
    }
    ancha.push(doc);
  }
  humo.ancha.insertMany(ancha);

  // --- humo_grande: does not fit the schema budget without -c ---------------------------------
  const grande = db.getSiblingDB("humo_grande");
  grande.dropDatabase();
  for (let c = 1; c <= 10; c++) {
    const docs = [];
    for (let i = 1; i <= 25; i++) {
      const doc = { _id: NumberInt(i) };
      for (let j = 1; j <= 39; j++) doc["indicador_trimestral_region_" + pad(j, 2)] = NumberInt(rand(100000));
      docs.push(doc);
    }
    grande.getCollection("g" + pad(c, 2)).insertMany(docs);
  }
  // valores: 30 string fields with 12 long frequent values each (~12 KB of value lists): with
  // -c valores the block only fits after the deterministic trimming of value lists.
  const valores = [];
  for (let i = 0; i < 48; i++) {
    const doc = { _id: NumberInt(i + 1) };
    for (let j = 1; j <= 30; j++) doc["v" + pad(j, 2)] = "valor-largo-de-prueba-v" + pad(j, 2) + "-" + pad(i % 12, 2);
    valores.push(doc);
  }
  grande.valores.insertMany(valores);

  // --- humo_muchas: more collections than are sampled without -c -------------------------------
  const muchas = db.getSiblingDB("humo_muchas");
  muchas.dropDatabase();
  for (let c = 1; c <= 51; c++) {
    muchas.getCollection("c" + pad(c, 2)).insertOne({ _id: NumberInt(1), x: NumberInt(c) });
  }

  print("humo: pedidos=" + humo.pedidos.countDocuments() + " clientes=" + humo.clientes.countDocuments() +
        " productos=" + humo.productos.countDocuments() + " ancha=" + humo.ancha.countDocuments() +
        " pedidos_madrid=" + humo.pedidos_madrid.countDocuments() +
        " humo_grande=" + grande.getCollectionNames().length + " humo_muchas=" +
        muchas.getCollectionNames().length + " lcg=" + state);
})();

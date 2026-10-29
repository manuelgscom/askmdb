// English demo database for the public recordings (scripts/grabar.sh). Synthetic and small.
//
//   mongosh --quiet demo-en.js   (against the test mongod; it drops and recreates "demo")
//
// Idempotent: drops and recreates the database "demo". Deterministic contents (a fixed-seed LCG,
// never Math.random); the only moving part is "today": order dates are days before the current
// UTC day, so questions like "last month" or "the last 3 months" always return rows.
//
//   demo  customers (60), products (24), orders (480, the last ~6 months)
//
// Every name is invented (first name + surname combinations, fictional product lines); there are
// no emails or phone numbers.

(function () {
  "use strict";

  // --- Deterministic helpers ------------------------------------------------------------------
  let state = 0x013528a5;
  function next() {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state;
  }
  function rand(n) {  // uniform integer in [0, n), from the high bits
    return Math.floor((next() / 4294967296) * n);
  }
  function pick(list) {
    return list[rand(list.length)];
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
  function money(c) {  // cents -> double with 2 decimals
    return Double(Math.round(c) / 100);
  }
  const DAY = 24 * 3600 * 1000;
  const now = new Date();
  const today = Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate());

  const demo = db.getSiblingDB("demo");
  demo.dropDatabase();

  // --- customers ------------------------------------------------------------------------------
  const first = ["Ava", "Liam", "Noah", "Mia", "Leo", "Zoe", "Ivan", "Nora", "Hugo", "Iris", "Owen", "Lena",
                 "Theo", "Ruby", "Felix", "Clara", "Milo", "Ada", "Oscar", "Elsa"];
  const last = ["Marlow", "Quill", "Brannock", "Vesper", "Holloway", "Thistle", "Ashdown", "Corrigan",
                "Penrose", "Wexley", "Fairbairn", "Calloway", "Dunmore", "Larkspur", "Redfield"];
  const places = [
    ["United States", ["Austin", "Denver", "Portland"]],
    ["United Kingdom", ["Leeds", "Bristol"]],
    ["Germany", ["Hamburg", "Leipzig"]],
    ["France", ["Lyon", "Nantes"]],
    ["Spain", ["Valencia", "Bilbao"]],
    ["Canada", ["Calgary", "Ottawa"]],
    ["Italy", ["Turin", "Bologna"]],
    ["Japan", ["Osaka", "Sapporo"]],
  ];
  const customers = [];
  const used = {};
  for (let i = 1; i <= 60; i++) {
    let name;
    do { name = pick(first) + " " + pick(last); } while (used[name]);
    used[name] = true;
    const place = weighted([[places[0], 22], [places[1], 14], [places[2], 14], [places[3], 12],
                            [places[4], 12], [places[5], 10], [places[6], 8], [places[7], 8]]);
    customers.push({
      _id: NumberInt(i),
      name: name,
      country: place[0],
      city: pick(place[1]),
      tier: weighted([["standard", 70], ["gold", 22], ["platinum", 8]]),
      signed_up: new Date(today - (200 + rand(1200)) * DAY),
    });
  }
  demo.customers.insertMany(customers);

  // --- products -------------------------------------------------------------------------------
  const catalog = [
    ["Aurora Desk Lamp", "home", 4900], ["Nimbus Throw Blanket", "home", 5900],
    ["Cedar Plant Stand", "home", 3400], ["Harbor Ceramic Mug", "kitchen", 1400],
    ["Ember Cast Pan", "kitchen", 6400], ["Tidal Water Bottle", "sports", 2200],
    ["Summit Daypack", "sports", 7900], ["Pulse Yoga Mat", "sports", 3900],
    ["Orbit Wireless Earbuds", "electronics", 12900], ["Quanta Power Bank", "electronics", 4500],
    ["Lumen Smart Bulb", "electronics", 1900], ["Vector Mechanical Keyboard", "electronics", 10900],
    ["Atlas Notebook A5", "office", 900], ["Glide Gel Pens (10)", "office", 1200],
    ["Pillar Monitor Stand", "office", 5400], ["Fable Hardcover Journal", "office", 2400],
    ["Breeze Linen Shirt", "clothing", 4400], ["Drift Wool Socks", "clothing", 1600],
    ["Canopy Rain Jacket", "clothing", 11900], ["Meadow Cotton Tee", "clothing", 2100],
    ["Spruce Cutting Board", "kitchen", 2900], ["Willow Tea Set", "kitchen", 4800],
    ["Comet Kids Scooter", "toys", 8900], ["Puzzle Box 1000", "toys", 1900],
  ];
  const products = [];
  for (let i = 0; i < catalog.length; i++) {
    products.push({
      _id: "P" + String(i + 1).padStart(3, "0"),
      name: catalog[i][0],
      category: catalog[i][1],
      price: money(catalog[i][2]),
      stock: NumberInt(rand(250)),
    });
  }
  demo.products.insertMany(products);

  // --- orders: 480 over the last 180 days, more recent ones slightly more frequent --------------
  const offsets = [];
  for (let i = 0; i < 480; i++) offsets.push(Math.min(rand(180), rand(200)));
  offsets.sort(function (a, b) { return b - a; });  // oldest first
  const orders = [];
  for (let i = 0; i < offsets.length; i++) {
    // A few loyal customers buy much more: a clear "top customers" list.
    const c = customers[rand(4) === 0 ? rand(8) : rand(customers.length)];
    const items = [];
    let total = 0;
    const n = 1 + rand(3);
    for (let k = 0; k < n; k++) {
      const p = products[rand(products.length)];
      const qty = 1 + rand(3);
      const cents = Math.round(p.price * 100);
      total += qty * cents;
      items.push({ product_id: p._id, quantity: NumberInt(qty), unit_price: p.price });
    }
    orders.push({
      _id: NumberInt(10001 + i),
      customer_id: c._id,
      created_at: new Date(today - offsets[i] * DAY + (8 + rand(13)) * 3600 * 1000 + rand(3600) * 1000),
      status: weighted([["delivered", 62], ["shipped", 14], ["pending", 12], ["cancelled", 12]]),
      payment: weighted([["card", 70], ["paypal", 22], ["bank_transfer", 8]]),
      items: items,
      total: money(total),
    });
  }
  demo.orders.insertMany(orders);

  demo.orders.createIndex({ created_at: 1 });
  demo.orders.createIndex({ customer_id: 1 });

  print("demo: customers " + demo.customers.countDocuments() + ", products " + demo.products.countDocuments() +
        ", orders " + demo.orders.countDocuments());
})();
